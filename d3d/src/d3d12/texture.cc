#include "d3d12/texture.h"

#include <vector>

namespace d3d12 {

Texture::Texture(ID3D12Device* device, DescriptorHeap* heap,
                 const D3D12_RESOURCE_DESC& desc, bool with_uav,
                 const wchar_t* name)
    : desc_(desc), heap_(heap), state_(kShaderResourceState) {
  CD3DX12_HEAP_PROPERTIES heap_properties(D3D12_HEAP_TYPE_DEFAULT);
  D3D12_CHECK(device->CreateCommittedResource(
      &heap_properties, D3D12_HEAP_FLAG_NONE, &desc_, state_, nullptr,
      IID_PPV_ARGS(&resource_)));
  resource_->SetName(name);

  srv_ = heap_->Allocate();
  D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
  srv_desc.Format = desc_.Format;
  srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  if (is_3d()) {
    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
    srv_desc.Texture3D.MipLevels = 1;
  } else {
    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MipLevels = 1;
  }
  device->CreateShaderResourceView(resource_.Get(), &srv_desc,
                                   heap_->cpu_handle(srv_));

  if (with_uav) {
    uav_ = heap_->Allocate();
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc = {};
    uav_desc.Format = desc_.Format;
    if (is_3d()) {
      uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
      uav_desc.Texture3D.WSize = desc_.DepthOrArraySize;
    } else {
      uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    }
    device->CreateUnorderedAccessView(resource_.Get(), nullptr, &uav_desc,
                                      heap_->cpu_handle(uav_));
  }
}

Texture::~Texture() { Release(); }

Texture::Texture(Texture&& other) noexcept { *this = std::move(other); }

Texture& Texture::operator=(Texture&& other) noexcept {
  if (this != &other) {
    Release();
    resource_ = std::move(other.resource_);
    desc_ = other.desc_;
    heap_ = std::exchange(other.heap_, nullptr);
    srv_ = std::exchange(other.srv_, UINT32_MAX);
    uav_ = std::exchange(other.uav_, UINT32_MAX);
    state_ = other.state_;
  }
  return *this;
}

void Texture::Release() {
  if (heap_ != nullptr) {
    if (srv_ != UINT32_MAX) {
      heap_->Free(srv_);
    }
    if (uav_ != UINT32_MAX) {
      heap_->Free(uav_);
    }
  }
  resource_.Reset();
  heap_ = nullptr;
  srv_ = UINT32_MAX;
  uav_ = UINT32_MAX;
}

Texture Texture::Create2d(ID3D12Device* device, DescriptorHeap* heap,
                          uint32_t width, uint32_t height, DXGI_FORMAT format,
                          bool with_uav, const wchar_t* name) {
  auto desc = CD3DX12_RESOURCE_DESC::Tex2D(
      format, width, height, 1, 1, 1, 0,
      with_uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
               : D3D12_RESOURCE_FLAG_NONE);
  return Texture(device, heap, desc, with_uav, name);
}

Texture Texture::Create3d(ID3D12Device* device, DescriptorHeap* heap,
                          uint32_t width, uint32_t height, uint32_t depth,
                          DXGI_FORMAT format, bool with_uav,
                          const wchar_t* name) {
  auto desc = CD3DX12_RESOURCE_DESC::Tex3D(
      format, width, height, static_cast<UINT16>(depth), 1,
      with_uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
               : D3D12_RESOURCE_FLAG_NONE);
  return Texture(device, heap, desc, with_uav, name);
}

bool Texture::GetTransition(D3D12_RESOURCE_STATES state,
                            D3D12_RESOURCE_BARRIER* barrier) {
  if (state == state_) {
    return false;
  }
  *barrier = CD3DX12_RESOURCE_BARRIER::Transition(resource_.Get(), state_,
                                                  state);
  state_ = state;
  return true;
}

void Transition(
    ID3D12GraphicsCommandList* command_list,
    std::initializer_list<std::pair<Texture*, D3D12_RESOURCE_STATES>>
        transitions) {
  std::vector<D3D12_RESOURCE_BARRIER> barriers;
  for (const auto& [texture, state] : transitions) {
    D3D12_RESOURCE_BARRIER barrier;
    if (texture->GetTransition(state, &barrier)) {
      barriers.push_back(barrier);
    }
  }
  if (!barriers.empty()) {
    command_list->ResourceBarrier(static_cast<UINT>(barriers.size()),
                                  barriers.data());
  }
}

void UavBarrier(ID3D12GraphicsCommandList* command_list) {
  auto barrier = CD3DX12_RESOURCE_BARRIER::UAV(nullptr);
  command_list->ResourceBarrier(1, &barrier);
}

}  // namespace d3d12
