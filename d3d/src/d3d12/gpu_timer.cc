#include "d3d12/gpu_timer.h"

#include <algorithm>

namespace d3d12 {

GpuTimer::GpuTimer(ID3D12Device* device, ID3D12CommandQueue* queue,
                   uint32_t max_sections)
    : max_sections_(max_sections) {
  D3D12_QUERY_HEAP_DESC desc = {};
  desc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
  desc.Count = 2 * max_sections;
  D3D12_CHECK(device->CreateQueryHeap(&desc, IID_PPV_ARGS(&query_heap_)));

  CD3DX12_HEAP_PROPERTIES heap_properties(D3D12_HEAP_TYPE_READBACK);
  auto buffer_desc =
      CD3DX12_RESOURCE_DESC::Buffer(sizeof(uint64_t) * 2 * max_sections);
  D3D12_CHECK(device->CreateCommittedResource(
      &heap_properties, D3D12_HEAP_FLAG_NONE, &buffer_desc,
      D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
      IID_PPV_ARGS(&readback_buffer_)));
  readback_buffer_->SetName(L"GpuTimer readback buffer");
  D3D12_CHECK(queue->GetTimestampFrequency(&frequency_));
}

void GpuTimer::Begin(ID3D12GraphicsCommandList* command_list,
                     const std::string& name) {
  if (section_names_.size() >= max_sections_) {
    throw std::runtime_error("Too many GpuTimer sections");
  }
  command_list->EndQuery(query_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                         static_cast<UINT>(2 * section_names_.size()));
  section_names_.push_back(name);
}

void GpuTimer::End(ID3D12GraphicsCommandList* command_list) {
  command_list->EndQuery(query_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                         static_cast<UINT>(2 * section_names_.size() - 1));
}

void GpuTimer::Resolve(ID3D12GraphicsCommandList* command_list) {
  if (!section_names_.empty()) {
    command_list->ResolveQueryData(
        query_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0,
        static_cast<UINT>(2 * section_names_.size()), readback_buffer_.Get(),
        0);
  }
}

std::vector<std::pair<std::string, double>> GpuTimer::ReadResults() {
  std::vector<std::pair<std::string, double>> results;
  if (section_names_.empty()) {
    return results;
  }
  const size_t count = 2 * section_names_.size();
  D3D12_RANGE read_range = {0, sizeof(uint64_t) * count};
  void* data;
  D3D12_CHECK(readback_buffer_->Map(0, &read_range, &data));
  const uint64_t* timestamps = static_cast<const uint64_t*>(data);
  for (size_t i = 0; i < section_names_.size(); ++i) {
    double milliseconds =
        static_cast<double>(timestamps[2 * i + 1] - timestamps[2 * i]) *
        1000.0 / static_cast<double>(frequency_);
    auto it = std::find_if(
        results.begin(), results.end(),
        [&](const auto& result) { return result.first == section_names_[i]; });
    if (it == results.end()) {
      results.emplace_back(section_names_[i], milliseconds);
    } else {
      it->second += milliseconds;
    }
  }
  D3D12_RANGE written_range = {0, 0};
  readback_buffer_->Unmap(0, &written_range);
  section_names_.clear();
  return results;
}

}  // namespace d3d12
