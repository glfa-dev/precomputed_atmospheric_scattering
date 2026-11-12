/*<h2>src/d3d12/device.h</h2>

<p>A minimal wrapper around a Direct3D 12 device and its direct command queue,
with a fence to synchronize the CPU with the GPU. The constructor checks that
the device supports the features required by this sample: Shader Model 6.6
dynamic resources (<code>ResourceDescriptorHeap</code>), and typed unordered
access view loads for 16 and 32 bits float RGBA textures (needed to accumulate
values in the precomputed textures).
*/

#ifndef D3D12_DEVICE_H_
#define D3D12_DEVICE_H_

#include <cstdint>
#include <cstdio>
#include <filesystem>

#include "d3d12/common.h"

namespace d3d12 {

class Device {
 public:
  // 'enable_debug_layer' enables the D3D12 debug layer, and
  // 'enable_gpu_validation' its GPU based validation (which is very slow). The
  // debug layer breaks into the debugger on errors, unless a log file is
  // specified, in which case all its messages are written to this file.
  Device(bool enable_debug_layer, bool enable_gpu_validation,
         const std::filesystem::path& log_file = {});
  ~Device();

  Device(const Device&) = delete;
  Device& operator=(const Device&) = delete;

  ID3D12Device* device() const { return device_.Get(); }
  IDXGIFactory6* factory() const { return factory_.Get(); }
  ID3D12CommandQueue* queue() const { return queue_.Get(); }
  const std::wstring& adapter_name() const { return adapter_name_; }

  // Signals the fence from the GPU queue, and returns the signaled value.
  uint64_t Signal();
  // Blocks the CPU until the GPU has reached the given fence value.
  void WaitForFenceValue(uint64_t value);
  // Blocks the CPU until the GPU has completed all the submitted work.
  void WaitIdle();
  // Closes, executes, and waits for the completion of the given command list.
  void ExecuteAndWait(ID3D12GraphicsCommandList* command_list);

 private:
  void CheckFeatureSupport();

  ComPtr<IDXGIFactory6> factory_;
  ComPtr<IDXGIAdapter1> adapter_;
  ComPtr<ID3D12Device> device_;
  ComPtr<ID3D12CommandQueue> queue_;
  ComPtr<ID3D12Fence> fence_;
  HANDLE fence_event_ = nullptr;
  uint64_t fence_value_ = 0;
  std::wstring adapter_name_;
  FILE* log_file_ = nullptr;
};

}  // namespace d3d12

#endif  // D3D12_DEVICE_H_
