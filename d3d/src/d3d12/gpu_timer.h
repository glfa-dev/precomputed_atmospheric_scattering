/*<h2>src/d3d12/gpu_timer.h</h2>

<p>Measures the GPU duration of named sections of a command list, with
timestamp queries. Sections with the same name are summed (e.g. to get the
total time spent in each precomputation pass, over all the scattering orders).
*/

#ifndef D3D12_GPU_TIMER_H_
#define D3D12_GPU_TIMER_H_

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "d3d12/common.h"

namespace d3d12 {

class GpuTimer {
 public:
  GpuTimer(ID3D12Device* device, ID3D12CommandQueue* queue,
           uint32_t max_sections);

  // Records a timestamp at the start / end of a named section. Sections must
  // not be nested.
  void Begin(ID3D12GraphicsCommandList* command_list, const std::string& name);
  void End(ID3D12GraphicsCommandList* command_list);
  // Records the resolution of the timestamps in a readback buffer. Must be
  // called after the last section, before closing the command list.
  void Resolve(ID3D12GraphicsCommandList* command_list);
  // Returns the total duration, in milliseconds, of each named section, in the
  // order of their first occurrence. Must be called after the GPU has executed
  // the command list. Also resets the recorded sections.
  std::vector<std::pair<std::string, double>> ReadResults();

 private:
  ComPtr<ID3D12QueryHeap> query_heap_;
  ComPtr<ID3D12Resource> readback_buffer_;
  uint32_t max_sections_;
  uint64_t frequency_;
  std::vector<std::string> section_names_;
};

}  // namespace d3d12

#endif  // D3D12_GPU_TIMER_H_
