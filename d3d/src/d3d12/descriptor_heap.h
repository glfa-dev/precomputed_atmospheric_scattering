/*<h2>src/d3d12/descriptor_heap.h</h2>

<p>A descriptor heap with a simple free list allocator. With Shader Model 6.6
dynamic resources, the index of a descriptor in the shader visible
CBV/SRV/UAV heap is all a shader needs to access the corresponding resource
(via <code>ResourceDescriptorHeap[index]</code>), so descriptors are identified
by their index.
*/

#ifndef D3D12_DESCRIPTOR_HEAP_H_
#define D3D12_DESCRIPTOR_HEAP_H_

#include <cstdint>
#include <vector>

#include "d3d12/common.h"

namespace d3d12 {

class DescriptorHeap {
 public:
  DescriptorHeap(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type,
                 uint32_t capacity, bool shader_visible, const wchar_t* name);

  DescriptorHeap(const DescriptorHeap&) = delete;
  DescriptorHeap& operator=(const DescriptorHeap&) = delete;

  // Returns the index of a free descriptor. Throws if the heap is full.
  uint32_t Allocate();
  // Releases a descriptor returned by Allocate. The caller must ensure that the
  // GPU no longer uses it.
  void Free(uint32_t index);

  ID3D12DescriptorHeap* heap() const { return heap_.Get(); }
  D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle(uint32_t index) const;
  D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle(uint32_t index) const;
  uint32_t IndexOf(D3D12_CPU_DESCRIPTOR_HANDLE handle) const;

 private:
  ComPtr<ID3D12DescriptorHeap> heap_;
  D3D12_CPU_DESCRIPTOR_HANDLE cpu_start_;
  D3D12_GPU_DESCRIPTOR_HANDLE gpu_start_ = {};
  uint32_t increment_;
  std::vector<uint32_t> free_indices_;
};

}  // namespace d3d12

#endif  // D3D12_DESCRIPTOR_HEAP_H_
