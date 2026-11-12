#include "d3d12/device.h"

#include <algorithm>

namespace d3d12 {

namespace {

void LogMessage(D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY severity,
                D3D12_MESSAGE_ID id, LPCSTR description, void* context) {
  static const char* kSeverities[] = {"CORRUPTION", "ERROR", "WARNING", "INFO",
                                      "MESSAGE"};
  FILE* file = static_cast<FILE*>(context);
  std::fprintf(file, "D3D12 %s #%d: %s\n",
               kSeverities[std::min<int>(severity, 4)], static_cast<int>(id),
               description);
  std::fflush(file);
}

}  // anonymous namespace

Device::Device(bool enable_debug_layer, bool enable_gpu_validation,
               const std::filesystem::path& log_file) {
  UINT factory_flags = 0;
  if (enable_debug_layer) {
    ComPtr<ID3D12Debug1> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
      debug->EnableDebugLayer();
      debug->SetEnableGPUBasedValidation(enable_gpu_validation);
      factory_flags |= DXGI_CREATE_FACTORY_DEBUG;
    }
  }
  D3D12_CHECK(CreateDXGIFactory2(factory_flags, IID_PPV_ARGS(&factory_)));

  // Use the first hardware adapter, by order of preference for performance,
  // which supports feature level 12.0.
  for (UINT i = 0; factory_->EnumAdapterByGpuPreference(
           i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
           IID_PPV_ARGS(&adapter_)) != DXGI_ERROR_NOT_FOUND;
       ++i) {
    DXGI_ADAPTER_DESC1 desc;
    adapter_->GetDesc1(&desc);
    if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
      continue;
    }
    if (SUCCEEDED(D3D12CreateDevice(adapter_.Get(), D3D_FEATURE_LEVEL_12_0,
                                    IID_PPV_ARGS(&device_)))) {
      adapter_name_ = desc.Description;
      break;
    }
  }
  if (!device_) {
    throw std::runtime_error("No Direct3D 12 hardware adapter found");
  }
  device_->SetName(L"Device");

  if (enable_debug_layer) {
    if (log_file.empty()) {
      // Break into the debugger on errors.
      ComPtr<ID3D12InfoQueue> info_queue;
      if (SUCCEEDED(device_.As(&info_queue))) {
        info_queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION,
                                       TRUE);
        info_queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE);
      }
    } else {
      // Write the debug layer messages to the log file instead.
      ComPtr<ID3D12InfoQueue1> info_queue;
      if (SUCCEEDED(device_.As(&info_queue)) &&
          _wfopen_s(&log_file_, log_file.c_str(), L"w") == 0) {
        DWORD cookie;
        info_queue->RegisterMessageCallback(
            LogMessage, D3D12_MESSAGE_CALLBACK_FLAG_NONE, log_file_, &cookie);
      }
    }
  }

  CheckFeatureSupport();

  D3D12_COMMAND_QUEUE_DESC queue_desc = {};
  queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  D3D12_CHECK(device_->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue_)));
  queue_->SetName(L"Direct queue");

  D3D12_CHECK(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                   IID_PPV_ARGS(&fence_)));
  fence_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (fence_event_ == nullptr) {
    throw std::runtime_error("CreateEvent failed");
  }
}

Device::~Device() {
  if (queue_) {
    WaitIdle();
  }
  if (fence_event_ != nullptr) {
    CloseHandle(fence_event_);
  }
  // Release the device before closing the log file used by its callback.
  queue_.Reset();
  fence_.Reset();
  device_.Reset();
  if (log_file_ != nullptr) {
    std::fclose(log_file_);
  }
}

void Device::CheckFeatureSupport() {
  D3D12_FEATURE_DATA_SHADER_MODEL shader_model = {D3D_SHADER_MODEL_6_6};
  if (FAILED(device_->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL,
                                          &shader_model,
                                          sizeof(shader_model))) ||
      shader_model.HighestShaderModel < D3D_SHADER_MODEL_6_6) {
    throw std::runtime_error("Shader Model 6.6 is not supported");
  }

  D3D12_FEATURE_DATA_D3D12_OPTIONS options = {};
  D3D12_CHECK(device_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS,
                                           &options, sizeof(options)));
  if (options.ResourceBindingTier < D3D12_RESOURCE_BINDING_TIER_3) {
    throw std::runtime_error("Resource binding tier 3 is not supported");
  }
  if (!options.TypedUAVLoadAdditionalFormats) {
    throw std::runtime_error("Typed UAV loads are not supported");
  }
  for (DXGI_FORMAT format : {DXGI_FORMAT_R16G16B16A16_FLOAT,
                             DXGI_FORMAT_R32G32B32A32_FLOAT}) {
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support = {format};
    D3D12_CHECK(device_->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,
                                             &support, sizeof(support)));
    if ((support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD) == 0) {
      throw std::runtime_error("Typed UAV loads of RGBA float textures are "
                               "not supported");
    }
  }
}

uint64_t Device::Signal() {
  D3D12_CHECK(queue_->Signal(fence_.Get(), ++fence_value_));
  return fence_value_;
}

void Device::WaitForFenceValue(uint64_t value) {
  if (fence_->GetCompletedValue() < value) {
    D3D12_CHECK(fence_->SetEventOnCompletion(value, fence_event_));
    WaitForSingleObject(fence_event_, INFINITE);
  }
}

void Device::WaitIdle() { WaitForFenceValue(Signal()); }

void Device::ExecuteAndWait(ID3D12GraphicsCommandList* command_list) {
  D3D12_CHECK(command_list->Close());
  ID3D12CommandList* command_lists[] = {command_list};
  queue_->ExecuteCommandLists(1, command_lists);
  WaitIdle();
}

}  // namespace d3d12
