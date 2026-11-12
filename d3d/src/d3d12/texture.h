/*<h2>src/d3d12/texture.h</h2>

<p>A texture resource together with its shader resource view (SRV) and,
optionally, its unordered access view (UAV) in the shader visible descriptor
heap, identified by their descriptor heap indices. The class also tracks the
current resource state, to record the transition barriers needed to use the
texture as a shader resource or as an unordered access view.
*/

#ifndef D3D12_TEXTURE_H_
#define D3D12_TEXTURE_H_

#include <cstdint>
#include <initializer_list>
#include <utility>

#include "d3d12/common.h"
#include "d3d12/descriptor_heap.h"

namespace d3d12 {

class Texture {
 public:
  // The state used for textures read by shaders (any stage).
  static constexpr D3D12_RESOURCE_STATES kShaderResourceState =
      D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;

  Texture() = default;
  ~Texture();

  Texture(Texture&& other) noexcept;
  Texture& operator=(Texture&& other) noexcept;
  Texture(const Texture&) = delete;
  Texture& operator=(const Texture&) = delete;

  // Creates a 2D texture with an SRV and, if 'with_uav' is true, a UAV.
  static Texture Create2d(ID3D12Device* device, DescriptorHeap* heap,
                          uint32_t width, uint32_t height, DXGI_FORMAT format,
                          bool with_uav, const wchar_t* name);
  // Creates a 3D texture with an SRV and, if 'with_uav' is true, a UAV.
  static Texture Create3d(ID3D12Device* device, DescriptorHeap* heap,
                          uint32_t width, uint32_t height, uint32_t depth,
                          DXGI_FORMAT format, bool with_uav,
                          const wchar_t* name);

  ID3D12Resource* resource() const { return resource_.Get(); }
  const D3D12_RESOURCE_DESC& desc() const { return desc_; }
  bool is_3d() const {
    return desc_.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
  }
  // The descriptor heap indices of the SRV and UAV of this texture.
  uint32_t srv() const { return srv_; }
  uint32_t uav() const { return uav_; }
  D3D12_RESOURCE_STATES state() const { return state_; }

  // Returns the barrier to transition this texture to 'state', and updates the
  // tracked state, or returns false if no transition is needed.
  bool GetTransition(D3D12_RESOURCE_STATES state,
                     D3D12_RESOURCE_BARRIER* barrier);

 private:
  Texture(ID3D12Device* device, DescriptorHeap* heap,
          const D3D12_RESOURCE_DESC& desc, bool with_uav, const wchar_t* name);
  void Release();

  ComPtr<ID3D12Resource> resource_;
  D3D12_RESOURCE_DESC desc_ = {};
  DescriptorHeap* heap_ = nullptr;
  uint32_t srv_ = UINT32_MAX;
  uint32_t uav_ = UINT32_MAX;
  D3D12_RESOURCE_STATES state_ = D3D12_RESOURCE_STATE_COMMON;
};

// Records the barriers to transition each texture to the associated state, in
// a single ResourceBarrier call.
void Transition(
    ID3D12GraphicsCommandList* command_list,
    std::initializer_list<std::pair<Texture*, D3D12_RESOURCE_STATES>>
        transitions);

// Records a barrier to wait for all the previous UAV accesses.
void UavBarrier(ID3D12GraphicsCommandList* command_list);

}  // namespace d3d12

#endif  // D3D12_TEXTURE_H_
