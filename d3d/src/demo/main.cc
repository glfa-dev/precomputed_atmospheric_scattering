/*<h2>src/demo/main.cc</h2>

<p>The entry point of the demo application. It creates a Win32 window, the
Direct3D 12 device, a flip model swap chain and the Dear ImGui context, and runs
the frame loop, with 2 frames in flight. The scene itself is rendered by the
<a href="demo.h.html">Demo</a> class.

<p>Command line options:
<ul>
<li><code>--gpu-validation</code> enables the GPU based validation of the D3D12
debug layer (the debug layer itself is always enabled in debug builds),</li>
<li><code>--keys &lt;characters&gt;</code> simulates the given keyboard
shortcuts at startup (e.g. <code>--keys 2l</code> selects the second predefined
view, and switches to approximate luminance mode),</li>
<li><code>--capture &lt;file.png&gt;</code> renders a few frames in a hidden
window, saves the last one in the given PNG file, and exits (useful for
automated tests - the D3D12 debug layer messages, if any, are then written in
a <code>&lt;file.png&gt;.log</code> file),</li>
<li><code>--capture-after &lt;seconds&gt;</code> delays the capture by the
given time (e.g. to test the shader hot reloading).</li>
</ul>
*/

// The DirectX-Headers d3d12.h must be included before the ImGui DX12 backend
// header (which includes d3d12.h).
#include "d3d12/common.h"

#include <imgui.h>
#include <imgui_impl_dx12.h>
#include <imgui_impl_win32.h>

#include <shellapi.h>
#include <wincodec.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "d3d12/descriptor_heap.h"
#include "d3d12/device.h"
#include "demo/demo.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param);

namespace {

using d3d12::ComPtr;

constexpr uint32_t kNumFrames = 2;
constexpr DXGI_FORMAT kSwapChainFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr uint32_t kDescriptorHeapCapacity = 256;
// The frame which is saved with the --capture option (the first frames are
// skipped so that the Dear ImGui windows have their final layout).
constexpr uint32_t kCaptureFrame = 4;

struct Options {
  bool enable_gpu_validation = false;
  std::string keys;
  std::wstring capture_path;
  double capture_after_seconds = 0.0;
};

Options ParseCommandLine() {
  Options options;
  int argc;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  for (int i = 1; i < argc; ++i) {
    std::wstring argument = argv[i];
    if (argument == L"--gpu-validation") {
      options.enable_gpu_validation = true;
    } else if (argument == L"--keys" && i + 1 < argc) {
      options.keys = d3d12::ToUtf8(argv[++i]);
    } else if (argument == L"--capture" && i + 1 < argc) {
      options.capture_path = std::filesystem::absolute(argv[++i]).wstring();
    } else if (argument == L"--capture-after" && i + 1 < argc) {
      options.capture_after_seconds = _wtof(argv[++i]);
    }
  }
  LocalFree(argv);
  return options;
}

// Saves the given RGBA 8 bits image in a PNG file, with the Windows Imaging
// Component. The alpha channel is ignored.
void SavePng(const std::wstring& path, uint32_t width, uint32_t height,
             uint32_t row_pitch, const uint8_t* rgba) {
  std::vector<uint8_t> bgra(4 * width * height);
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      const uint8_t* src = rgba + y * row_pitch + 4 * x;
      uint8_t* dst = bgra.data() + 4 * (y * width + x);
      dst[0] = src[2];
      dst[1] = src[1];
      dst[2] = src[0];
      dst[3] = 255;
    }
  }
  D3D12_CHECK(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
  {
    ComPtr<IWICImagingFactory> factory;
    D3D12_CHECK(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                 CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)));
    ComPtr<IWICStream> stream;
    D3D12_CHECK(factory->CreateStream(&stream));
    D3D12_CHECK(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE));
    ComPtr<IWICBitmapEncoder> encoder;
    D3D12_CHECK(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr,
                                       &encoder));
    D3D12_CHECK(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache));
    ComPtr<IWICBitmapFrameEncode> frame;
    D3D12_CHECK(encoder->CreateNewFrame(&frame, nullptr));
    D3D12_CHECK(frame->Initialize(nullptr));
    D3D12_CHECK(frame->SetSize(width, height));
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    D3D12_CHECK(frame->SetPixelFormat(&format));
    if (format != GUID_WICPixelFormat32bppBGRA) {
      throw std::runtime_error("Unsupported PNG pixel format");
    }
    D3D12_CHECK(frame->WritePixels(height, 4 * width,
                                   static_cast<UINT>(bgra.size()),
                                   bgra.data()));
    D3D12_CHECK(frame->Commit());
    D3D12_CHECK(encoder->Commit());
  }
  CoUninitialize();
}

// Returns the shader directory: the source directory if it exists (so that
// shader changes are reloaded at runtime), or the "shaders" directory next to
// the executable otherwise.
std::filesystem::path GetShaderDirectory() {
  std::filesystem::path source_directory = ATMOSPHERE_SHADER_SOURCE_DIRECTORY;
  if (std::filesystem::exists(source_directory / "atmosphere" /
                              "functions.hlsli")) {
    return source_directory;
  }
  wchar_t path[MAX_PATH];
  GetModuleFileNameW(nullptr, path, MAX_PATH);
  return std::filesystem::path(path).parent_path() / "shaders";
}

class App {
 public:
  App(HINSTANCE instance, const Options& options);
  ~App();

  int Run();
  LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM w_param,
                        LPARAM l_param);

 private:
  void CreateRenderTargets();
  void Resize(uint32_t width, uint32_t height);
  void RenderFrame();
  // Records the copy of the given render target in capture_buffer_.
  void RecordCapture(ID3D12Resource* render_target);
  void SaveCapture();

  Options options_;
  bool done_ = false;
  uint32_t frame_count_ = 0;
  ULONGLONG start_time_ = GetTickCount64();
  HWND hwnd_ = nullptr;
  uint32_t width_ = 1280;
  uint32_t height_ = 720;
  std::unique_ptr<d3d12::Device> device_;
  std::unique_ptr<d3d12::DescriptorHeap> srv_heap_;
  std::unique_ptr<d3d12::DescriptorHeap> rtv_heap_;
  ComPtr<IDXGISwapChain3> swap_chain_;
  std::array<ComPtr<ID3D12Resource>, kNumFrames> render_targets_;
  std::array<uint32_t, kNumFrames> render_target_views_;
  std::array<ComPtr<ID3D12CommandAllocator>, kNumFrames> allocators_;
  std::array<uint64_t, kNumFrames> frame_fence_values_ = {};
  ComPtr<ID3D12GraphicsCommandList> command_list_;
  std::unique_ptr<atmosphere::demo::Demo> demo_;
  uint32_t frame_index_ = 0;
  bool dragging_ = false;
  bool drag_moves_sun_ = false;
  POINT previous_mouse_position_ = {};
  ComPtr<ID3D12Resource> capture_buffer_;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT capture_footprint_ = {};
};

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM w_param,
                            LPARAM l_param) {
  if (message == WM_NCCREATE) {
    auto* create_struct = reinterpret_cast<CREATESTRUCTW*>(l_param);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA,
        reinterpret_cast<LONG_PTR>(create_struct->lpCreateParams));
  }
  auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (app != nullptr) {
    return app->HandleMessage(hwnd, message, w_param, l_param);
  }
  return DefWindowProcW(hwnd, message, w_param, l_param);
}

App::App(HINSTANCE instance, const Options& options) : options_(options) {
#ifdef NDEBUG
  const bool enable_debug_layer = options.enable_gpu_validation;
#else
  const bool enable_debug_layer = true;
#endif
  // In capture mode, the debug layer messages are written to a log file next
  // to the captured image, instead of breaking into the debugger.
  device_ = std::make_unique<d3d12::Device>(
      enable_debug_layer, options.enable_gpu_validation,
      options.capture_path.empty()
          ? std::filesystem::path()
          : std::filesystem::path(options.capture_path + L".log"));
  ID3D12Device* device = device_->device();
  srv_heap_ = std::make_unique<d3d12::DescriptorHeap>(
      device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kDescriptorHeapCapacity,
      true, L"Shader visible CBV/SRV/UAV heap");
  rtv_heap_ = std::make_unique<d3d12::DescriptorHeap>(
      device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kNumFrames, false, L"RTV heap");

  // Create the window.
  WNDCLASSEXW window_class = {};
  window_class.cbSize = sizeof(window_class);
  window_class.style = CS_HREDRAW | CS_VREDRAW;
  window_class.lpfnWndProc = WindowProc;
  window_class.hInstance = instance;
  window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
  window_class.lpszClassName = L"AtmosphereDemo";
  RegisterClassExW(&window_class);
  RECT rect = {0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_)};
  AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
  hwnd_ = CreateWindowExW(
      0, window_class.lpszClassName,
      L"Precomputed Atmospheric Scattering - Direct3D 12", WS_OVERLAPPEDWINDOW,
      CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left,
      rect.bottom - rect.top, nullptr, nullptr, instance, this);
  if (hwnd_ == nullptr) {
    throw std::runtime_error("CreateWindow failed");
  }

  // Create the swap chain and its render target views.
  DXGI_SWAP_CHAIN_DESC1 swap_chain_desc = {};
  swap_chain_desc.Width = width_;
  swap_chain_desc.Height = height_;
  swap_chain_desc.Format = kSwapChainFormat;
  swap_chain_desc.SampleDesc.Count = 1;
  swap_chain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  swap_chain_desc.BufferCount = kNumFrames;
  swap_chain_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  ComPtr<IDXGISwapChain1> swap_chain;
  D3D12_CHECK(device_->factory()->CreateSwapChainForHwnd(
      device_->queue(), hwnd_, &swap_chain_desc, nullptr, nullptr,
      &swap_chain));
  D3D12_CHECK(device_->factory()->MakeWindowAssociation(
      hwnd_, DXGI_MWA_NO_ALT_ENTER));
  D3D12_CHECK(swap_chain.As(&swap_chain_));
  for (uint32_t i = 0; i < kNumFrames; ++i) {
    render_target_views_[i] = rtv_heap_->Allocate();
  }
  CreateRenderTargets();

  for (uint32_t i = 0; i < kNumFrames; ++i) {
    D3D12_CHECK(device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocators_[i])));
  }
  D3D12_CHECK(device->CreateCommandList(
      0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators_[0].Get(), nullptr,
      IID_PPV_ARGS(&command_list_)));
  D3D12_CHECK(command_list_->Close());

  // Initialize Dear ImGui, with descriptors allocated in our shader visible
  // heap.
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  if (!options_.capture_path.empty()) {
    // Use the default window layout for captures.
    ImGui::GetIO().IniFilename = nullptr;
  }
  ImGui::StyleColorsDark();
  ImGui_ImplWin32_Init(hwnd_);
  ImGui_ImplDX12_InitInfo init_info;
  init_info.Device = device;
  init_info.CommandQueue = device_->queue();
  init_info.NumFramesInFlight = kNumFrames;
  init_info.RTVFormat = kSwapChainFormat;
  init_info.DSVFormat = DXGI_FORMAT_UNKNOWN;
  init_info.UserData = srv_heap_.get();
  init_info.SrvDescriptorHeap = srv_heap_->heap();
  init_info.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo* info,
                                      D3D12_CPU_DESCRIPTOR_HANDLE* cpu_handle,
                                      D3D12_GPU_DESCRIPTOR_HANDLE* gpu_handle) {
    auto* heap = static_cast<d3d12::DescriptorHeap*>(info->UserData);
    uint32_t index = heap->Allocate();
    *cpu_handle = heap->cpu_handle(index);
    *gpu_handle = heap->gpu_handle(index);
  };
  init_info.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo* info,
                                     D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle,
                                     D3D12_GPU_DESCRIPTOR_HANDLE) {
    auto* heap = static_cast<d3d12::DescriptorHeap*>(info->UserData);
    heap->Free(heap->IndexOf(cpu_handle));
  };
  ImGui_ImplDX12_Init(&init_info);

  demo_ = std::make_unique<atmosphere::demo::Demo>(
      device_.get(), srv_heap_.get(), GetShaderDirectory(), kNumFrames);
  for (char key : options_.keys) {
    demo_->HandleKey(key);
  }

  // In capture mode, the window stays hidden.
  if (options_.capture_path.empty()) {
    ShowWindow(hwnd_, SW_SHOWDEFAULT);
  }
}

App::~App() {
  device_->WaitIdle();
  demo_.reset();
  ImGui_ImplDX12_Shutdown();
  ImGui_ImplWin32_Shutdown();
  ImGui::DestroyContext();
  if (hwnd_ != nullptr) {
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
    DestroyWindow(hwnd_);
  }
}

void App::CreateRenderTargets() {
  for (uint32_t i = 0; i < kNumFrames; ++i) {
    D3D12_CHECK(swap_chain_->GetBuffer(i, IID_PPV_ARGS(&render_targets_[i])));
    device_->device()->CreateRenderTargetView(
        render_targets_[i].Get(), nullptr,
        rtv_heap_->cpu_handle(render_target_views_[i]));
  }
}

void App::Resize(uint32_t width, uint32_t height) {
  if (width == 0 || height == 0 || (width == width_ && height == height_)) {
    return;
  }
  device_->WaitIdle();
  for (auto& render_target : render_targets_) {
    render_target.Reset();
  }
  D3D12_CHECK(swap_chain_->ResizeBuffers(kNumFrames, width, height,
                                         kSwapChainFormat, 0));
  width_ = width;
  height_ = height;
  CreateRenderTargets();
}

int App::Run() {
  MSG message = {};
  while (true) {
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      if (message.message == WM_QUIT) {
        return static_cast<int>(message.wParam);
      }
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    if (done_) {
      return 0;
    }
    if (IsIconic(hwnd_)) {
      Sleep(10);
      continue;
    }
    RenderFrame();
  }
}

void App::RenderFrame() {
  // Wait until the GPU has finished using the resources of this frame.
  const uint32_t back_buffer = swap_chain_->GetCurrentBackBufferIndex();
  device_->WaitForFenceValue(frame_fence_values_[frame_index_]);

  demo_->Update();
  ImGui_ImplDX12_NewFrame();
  ImGui_ImplWin32_NewFrame();
  ImGui::NewFrame();
  demo_->DrawUi();
  ImGui::Render();

  ID3D12CommandAllocator* allocator = allocators_[frame_index_].Get();
  D3D12_CHECK(allocator->Reset());
  D3D12_CHECK(command_list_->Reset(allocator, nullptr));
  ID3D12DescriptorHeap* heaps[] = {srv_heap_->heap()};
  command_list_->SetDescriptorHeaps(1, heaps);

  auto to_render_target = CD3DX12_RESOURCE_BARRIER::Transition(
      render_targets_[back_buffer].Get(), D3D12_RESOURCE_STATE_PRESENT,
      D3D12_RESOURCE_STATE_RENDER_TARGET);
  command_list_->ResourceBarrier(1, &to_render_target);
  D3D12_CPU_DESCRIPTOR_HANDLE rtv =
      rtv_heap_->cpu_handle(render_target_views_[back_buffer]);
  demo_->Render(command_list_.Get(), frame_index_, rtv, width_, height_);
  command_list_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), command_list_.Get());
  const bool capture =
      !options_.capture_path.empty() && ++frame_count_ >= kCaptureFrame &&
      (GetTickCount64() - start_time_) >=
          options_.capture_after_seconds * 1000.0;
  if (capture) {
    RecordCapture(render_targets_[back_buffer].Get());
  }
  auto to_present = CD3DX12_RESOURCE_BARRIER::Transition(
      render_targets_[back_buffer].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
      D3D12_RESOURCE_STATE_PRESENT);
  command_list_->ResourceBarrier(1, &to_present);
  D3D12_CHECK(command_list_->Close());

  ID3D12CommandList* command_lists[] = {command_list_.Get()};
  device_->queue()->ExecuteCommandLists(1, command_lists);
  D3D12_CHECK(swap_chain_->Present(1, 0));
  frame_fence_values_[frame_index_] = device_->Signal();
  frame_index_ = (frame_index_ + 1) % kNumFrames;
  if (capture) {
    device_->WaitIdle();
    SaveCapture();
    done_ = true;
  }
}

void App::RecordCapture(ID3D12Resource* render_target) {
  ID3D12Device* device = device_->device();
  D3D12_RESOURCE_DESC desc = render_target->GetDesc();
  UINT64 total_size;
  device->GetCopyableFootprints(&desc, 0, 1, 0, &capture_footprint_, nullptr,
                                nullptr, &total_size);
  CD3DX12_HEAP_PROPERTIES readback_heap(D3D12_HEAP_TYPE_READBACK);
  auto buffer_desc = CD3DX12_RESOURCE_DESC::Buffer(total_size);
  D3D12_CHECK(device->CreateCommittedResource(
      &readback_heap, D3D12_HEAP_FLAG_NONE, &buffer_desc,
      D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
      IID_PPV_ARGS(&capture_buffer_)));
  auto to_copy_source = CD3DX12_RESOURCE_BARRIER::Transition(
      render_target, D3D12_RESOURCE_STATE_RENDER_TARGET,
      D3D12_RESOURCE_STATE_COPY_SOURCE);
  command_list_->ResourceBarrier(1, &to_copy_source);
  CD3DX12_TEXTURE_COPY_LOCATION destination(capture_buffer_.Get(),
                                            capture_footprint_);
  CD3DX12_TEXTURE_COPY_LOCATION source(render_target, 0);
  command_list_->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
  auto to_render_target = CD3DX12_RESOURCE_BARRIER::Transition(
      render_target, D3D12_RESOURCE_STATE_COPY_SOURCE,
      D3D12_RESOURCE_STATE_RENDER_TARGET);
  command_list_->ResourceBarrier(1, &to_render_target);
}

void App::SaveCapture() {
  void* data;
  D3D12_CHECK(capture_buffer_->Map(0, nullptr, &data));
  SavePng(options_.capture_path, capture_footprint_.Footprint.Width,
          capture_footprint_.Footprint.Height,
          capture_footprint_.Footprint.RowPitch,
          static_cast<const uint8_t*>(data));
  capture_buffer_->Unmap(0, nullptr);
}

LRESULT App::HandleMessage(HWND hwnd, UINT message, WPARAM w_param,
                           LPARAM l_param) {
  if (ImGui::GetCurrentContext() != nullptr &&
      ImGui_ImplWin32_WndProcHandler(hwnd, message, w_param, l_param)) {
    return true;
  }
  const bool imgui_wants_mouse = ImGui::GetCurrentContext() != nullptr &&
                                 ImGui::GetIO().WantCaptureMouse;
  const bool imgui_wants_keyboard = ImGui::GetCurrentContext() != nullptr &&
                                    ImGui::GetIO().WantCaptureKeyboard;
  switch (message) {
    case WM_SIZE:
      if (swap_chain_ && w_param != SIZE_MINIMIZED) {
        Resize(LOWORD(l_param), HIWORD(l_param));
      }
      return 0;
    case WM_LBUTTONDOWN:
      if (!imgui_wants_mouse) {
        dragging_ = true;
        drag_moves_sun_ = (w_param & MK_CONTROL) != 0;
        previous_mouse_position_ = {static_cast<short>(LOWORD(l_param)),
                                    static_cast<short>(HIWORD(l_param))};
        SetCapture(hwnd);
      }
      return 0;
    case WM_LBUTTONUP:
      if (dragging_) {
        dragging_ = false;
        ReleaseCapture();
      }
      return 0;
    case WM_MOUSEMOVE:
      if (dragging_ && demo_) {
        POINT position = {static_cast<short>(LOWORD(l_param)),
                          static_cast<short>(HIWORD(l_param))};
        demo_->HandleMouseDrag(position.x - previous_mouse_position_.x,
                               position.y - previous_mouse_position_.y,
                               drag_moves_sun_);
        previous_mouse_position_ = position;
      }
      return 0;
    case WM_MOUSEWHEEL:
      if (!imgui_wants_mouse && demo_) {
        demo_->HandleMouseWheel(GET_WHEEL_DELTA_WPARAM(w_param) > 0 ? 1 : -1);
      }
      return 0;
    case WM_CHAR:
      if (!imgui_wants_keyboard && demo_) {
        if (w_param == VK_ESCAPE) {
          PostMessageW(hwnd, WM_CLOSE, 0, 0);
        } else if (w_param < 128) {
          demo_->HandleKey(static_cast<char>(w_param));
        }
      }
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(hwnd, message, w_param, l_param);
}

}  // anonymous namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
  const Options options = ParseCommandLine();
  try {
    App app(instance, options);
    return app.Run();
  } catch (const std::exception& error) {
    if (options.capture_path.empty()) {
      MessageBoxA(nullptr, error.what(), "Atmosphere demo error",
                  MB_OK | MB_ICONERROR);
    } else {
      OutputDebugStringA(error.what());
    }
    return 1;
  }
}
