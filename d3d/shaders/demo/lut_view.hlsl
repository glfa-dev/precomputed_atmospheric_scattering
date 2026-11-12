/*<h2>shaders/demo/lut_view.hlsl</h2>

<p>This compute shader is used by the demo's "LUT viewer" window. It copies a
precomputed 2D texture, or a slice of a precomputed 3D texture, into an 8 bits
RGBA texture which can be displayed by Dear ImGui. The values are multiplied by
a user defined scale factor, clamped to [0,1], and gamma corrected. The texture
is flipped vertically so that, as in OpenGL, the first texel row is displayed at
the bottom (i.e. $r$ increases upwards).
*/

#define LUT_VIEW_ROOT_SIGNATURE \
    "RootFlags(CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED)," \
    "RootConstants(num32BitConstants=8, b0)"

struct LutViewParameters {
  // Descriptor heap index of the source texture SRV.
  uint source_texture;
  // Whether the source texture is a 3D texture.
  uint is_3d;
  // The slice to display, for 3D textures.
  uint slice;
  // 0 to display the RGB channels, 1 to display the alpha channel.
  uint show_alpha;
  float scale;
  // Descriptor heap index of the destination texture UAV.
  uint output_texture;
  uint width;
  uint height;
};

ConstantBuffer<LutViewParameters> parameters : register(b0);

[RootSignature(LUT_VIEW_ROOT_SIGNATURE)]
[numthreads(8, 8, 1)]
void LutViewCS(uint3 texel : SV_DispatchThreadID) {
  if (any(texel.xy >= uint2(parameters.width, parameters.height))) {
    return;
  }
  int2 source_texel = int2(texel.x, parameters.height - 1 - texel.y);
  float4 value;
  if (parameters.is_3d != 0) {
    Texture3D<float4> source =
        ResourceDescriptorHeap[parameters.source_texture];
    value = source.Load(int4(source_texel, parameters.slice, 0));
  } else {
    Texture2D<float4> source =
        ResourceDescriptorHeap[parameters.source_texture];
    value = source.Load(int3(source_texel, 0));
  }
  float3 color = parameters.show_alpha != 0 ? value.aaa : value.rgb;
  RWTexture2D<float4> output =
      ResourceDescriptorHeap[parameters.output_texture];
  output[texel.xy] =
      float4(pow(saturate(color * parameters.scale), 1.0 / 2.2), 1.0);
}
