#include "d3d12/descriptor_heap.h"

namespace d3d12 {

DescriptorHeap::DescriptorHeap(ID3D12Device* device,
                               D3D12_DESCRIPTOR_HEAP_TYPE type,
                               uint32_t capacity, bool shader_visible,
                               const wchar_t* name) {
  D3D12_DESCRIPTOR_HEAP_DESC desc = {};
  desc.Type = type;
  desc.NumDescriptors = capacity;
  desc.Flags = shader_visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
                              : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
  D3D12_CHECK(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap_)));
  heap_->SetName(name);
  cpu_start_ = heap_->GetCPUDescriptorHandleForHeapStart();
  if (shader_visible) {
    gpu_start_ = heap_->GetGPUDescriptorHandleForHeapStart();
  }
  increment_ = device->GetDescriptorHandleIncrementSize(type);
  // Allocate the lowest indices first.
  free_indices_.reserve(capacity);
  for (uint32_t i = capacity; i > 0; --i) {
    free_indices_.push_back(i - 1);
  }
}

uint32_t DescriptorHeap::Allocate() {
  if (free_indices_.empty()) {
    throw std::runtime_error("Descriptor heap is full");
  }
  uint32_t index = free_indices_.back();
  free_indices_.pop_back();
  return index;
}

void DescriptorHeap::Free(uint32_t index) { free_indices_.push_back(index); }

D3D12_CPU_DESCRIPTOR_HANDLE DescriptorHeap::cpu_handle(uint32_t index) const {
  return {cpu_start_.ptr + static_cast<SIZE_T>(index) * increment_};
}

D3D12_GPU_DESCRIPTOR_HANDLE DescriptorHeap::gpu_handle(uint32_t index) const {
  return {gpu_start_.ptr + static_cast<UINT64>(index) * increment_};
}

uint32_t DescriptorHeap::IndexOf(D3D12_CPU_DESCRIPTOR_HANDLE handle) const {
  return static_cast<uint32_t>((handle.ptr - cpu_start_.ptr) / increment_);
}

}  // namespace d3d12
