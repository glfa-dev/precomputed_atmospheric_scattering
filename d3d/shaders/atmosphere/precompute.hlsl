/**
 * Copyright (c) 2017 Eric Bruneton
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holders nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

/*<h2>shaders/atmosphere/precompute.hlsl</h2>

<p>This file contains the compute shaders which precompute the transmittance,
scattering and irradiance textures. They are the HLSL equivalent of the
<code>kCompute*Shader</code> GLSL fragment shaders of
<code>atmosphere/model.cc</code>, and simply wrap the precomputation functions
from <a href="functions.hlsli.html">functions.hlsli</a> in complete shaders.
They are used by the C++ <code>atmosphere::Model</code> class
(<a href="../../src/atmosphere/model.cc.html">model.cc</a>), which compiles each
entry point of this file separately, with the following preprocessor macros:
<ul>
<li>the texture sizes (see <a href="definitions.hlsli.html">definitions.hlsli
</a>),</li>
<li><code>COMBINED_SCATTERING_TEXTURES</code>, if the single Mie scattering must
be packed in the alpha channel of the scattering texture.</li>
</ul>

<p>Compared to the GLSL version, which renders one quad per texture layer, with
a geometry shader to select the layer, a single dispatch computes a whole 2D or
3D texture here. Another difference is that the GLSL version uses hardware
blending to accumulate the contributions of each scattering order (and of each
group of 3 wavelengths, in precomputed illuminance mode) in the final textures.
Here this is done explicitly, with a read-modify-write of each output texel,
controlled by a bit mask (bit <i>i</i> enables blending for output <i>i</i>).

<h3>Resources</h3>

<p>All the shaders in this file use the same root signature, declared in HLSL
so that the C++ code can create it directly from the compiled shader. It uses
<a href="https://microsoft.github.io/DirectX-Specs/d3d/HLSL_SM_6_6_DynamicResources.html"
>Shader Model 6.6 dynamic resources</a>: the textures are accessed via indices
in the shader visible descriptor heap, provided with root constants.
*/

#define PRECOMPUTE_ROOT_SIGNATURE \
    "RootFlags(CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED)," \
    "CBV(b0)," \
    "RootConstants(num32BitConstants=12, b1)," \
    "StaticSampler(s0," \
    "    filter=FILTER_MIN_MAG_MIP_LINEAR," \
    "    addressU=TEXTURE_ADDRESS_CLAMP," \
    "    addressV=TEXTURE_ADDRESS_CLAMP," \
    "    addressW=TEXTURE_ADDRESS_CLAMP)"

#include "functions.hlsli"

/*
<p>The constant buffer contains the atmosphere parameters, specialized for the 3
wavelengths currently being precomputed, and a matrix which converts radiance
values to luminance values (the identity in precomputed radiance mode - see
the <code>Model::Init</code> method for more details). Note that this matrix is
uploaded row by row (each row being padded to 16 bytes), and that a matrix
times vector product is written <code>mul(matrix, vector)</code> in HLSL:
*/

struct PrecomputeConstants {
  AtmosphereParameters atmosphere;
  row_major float3x3 luminance_from_radiance;
};

/*
<p>The root constants contain the descriptor heap indices of the input textures
(shader resource views) and of the output textures (unordered access views), as
well as the current scattering order and the blending bit mask. Each shader only
uses a subset of these indices:
*/

struct PrecomputeResources {
  uint transmittance_texture;
  uint single_rayleigh_scattering_texture;
  uint single_mie_scattering_texture;
  uint multiple_scattering_texture;
  uint irradiance_texture;
  uint scattering_density_texture;
  uint output0;
  uint output1;
  uint output2;
  uint output3;
  int scattering_order;
  uint blend;
};

ConstantBuffer<PrecomputeConstants> constants : register(b0);
ConstantBuffer<PrecomputeResources> resources : register(b1);
SamplerState lut_sampler : register(s0);

/*
<p>The following helper functions write a value in an output texture, or add it
to the current value, depending on the blending bit mask:
*/

void Store(RWTexture2D<float4> output, uint index, uint2 texel, float4 value) {
  output[texel] =
      (resources.blend & (1u << index)) != 0 ? output[texel] + value : value;
}

void Store(RWTexture3D<float4> output, uint index, uint3 texel, float4 value) {
  output[texel] =
      (resources.blend & (1u << index)) != 0 ? output[texel] + value : value;
}

/*
<h3>Shaders</h3>

<p>Each shader runs one thread per output texel. All the precomputed texture
sizes are multiples of 8 but, to be safe, the threads outside the texture are
discarded anyway. The texel center coordinates, <code>texel + 0.5</code>, are
the equivalent of <code>gl_FragCoord</code> in the GLSL shaders.

<p>The transmittance shader writes the transmittance texture:
*/

[RootSignature(PRECOMPUTE_ROOT_SIGNATURE)]
[numthreads(8, 8, 1)]
void ComputeTransmittanceCS(uint3 texel : SV_DispatchThreadID) {
  if (any(texel.xy >= uint2(TRANSMITTANCE_TEXTURE_WIDTH,
                            TRANSMITTANCE_TEXTURE_HEIGHT))) {
    return;
  }
  RWTexture2D<float4> transmittance = ResourceDescriptorHeap[resources.output0];
  Store(transmittance, 0, texel.xy, float4(
      ComputeTransmittanceToTopAtmosphereBoundaryTexture(
          constants.atmosphere, texel.xy + 0.5), 1.0));
}

/*
<p>The direct irradiance shader writes the direct irradiance in the delta
irradiance texture, and initializes the irradiance texture with zeros (or leaves
it unchanged, when blending is enabled):
*/

[RootSignature(PRECOMPUTE_ROOT_SIGNATURE)]
[numthreads(8, 8, 1)]
void ComputeDirectIrradianceCS(uint3 texel : SV_DispatchThreadID) {
  if (any(texel.xy >= uint2(IRRADIANCE_TEXTURE_WIDTH,
                            IRRADIANCE_TEXTURE_HEIGHT))) {
    return;
  }
  Texture2D<float4> transmittance_texture =
      ResourceDescriptorHeap[resources.transmittance_texture];
  RWTexture2D<float4> delta_irradiance =
      ResourceDescriptorHeap[resources.output0];
  RWTexture2D<float4> irradiance = ResourceDescriptorHeap[resources.output1];
  Store(delta_irradiance, 0, texel.xy, float4(
      ComputeDirectIrradianceTexture(constants.atmosphere,
          transmittance_texture, lut_sampler, texel.xy + 0.5), 1.0));
  Store(irradiance, 1, texel.xy, (float4)0.0);
}

/*
<p>The single scattering shader writes the Rayleigh and Mie single scattering
in the delta Rayleigh and delta Mie scattering textures, and writes (or
accumulates) them, converted to luminance, in the scattering texture and, if
the textures are not combined, in the single Mie scattering texture:
*/

[RootSignature(PRECOMPUTE_ROOT_SIGNATURE)]
[numthreads(8, 8, 1)]
void ComputeSingleScatteringCS(uint3 texel : SV_DispatchThreadID) {
  if (any(texel >= uint3(SCATTERING_TEXTURE_WIDTH, SCATTERING_TEXTURE_HEIGHT,
                         SCATTERING_TEXTURE_DEPTH))) {
    return;
  }
  Texture2D<float4> transmittance_texture =
      ResourceDescriptorHeap[resources.transmittance_texture];
  RWTexture3D<float4> delta_rayleigh =
      ResourceDescriptorHeap[resources.output0];
  RWTexture3D<float4> delta_mie = ResourceDescriptorHeap[resources.output1];
  RWTexture3D<float4> scattering = ResourceDescriptorHeap[resources.output2];

  float3 rayleigh;
  float3 mie;
  ComputeSingleScatteringTexture(constants.atmosphere, transmittance_texture,
      lut_sampler, texel + 0.5, rayleigh, mie);
  float3x3 luminance_from_radiance = constants.luminance_from_radiance;
  Store(delta_rayleigh, 0, texel, float4(rayleigh, 1.0));
  Store(delta_mie, 1, texel, float4(mie, 1.0));
  Store(scattering, 2, texel, float4(mul(luminance_from_radiance, rayleigh),
      mul(luminance_from_radiance, mie).r));
#ifndef COMBINED_SCATTERING_TEXTURES
  RWTexture3D<float4> single_mie_scattering =
      ResourceDescriptorHeap[resources.output3];
  Store(single_mie_scattering, 3, texel,
      float4(mul(luminance_from_radiance, mie), 1.0));
#endif
}

/*
<p>The scattering density shader computes the scattering density for the
current scattering order, from the single scattering textures (for order 2), or
the multiple scattering texture of the previous order (for orders 3 or more),
and from the ground irradiance texture of the previous order:
*/

[RootSignature(PRECOMPUTE_ROOT_SIGNATURE)]
[numthreads(8, 8, 1)]
void ComputeScatteringDensityCS(uint3 texel : SV_DispatchThreadID) {
  if (any(texel >= uint3(SCATTERING_TEXTURE_WIDTH, SCATTERING_TEXTURE_HEIGHT,
                         SCATTERING_TEXTURE_DEPTH))) {
    return;
  }
  Texture2D<float4> transmittance_texture =
      ResourceDescriptorHeap[resources.transmittance_texture];
  Texture3D<float4> single_rayleigh_scattering_texture =
      ResourceDescriptorHeap[resources.single_rayleigh_scattering_texture];
  Texture3D<float4> single_mie_scattering_texture =
      ResourceDescriptorHeap[resources.single_mie_scattering_texture];
  Texture3D<float4> multiple_scattering_texture =
      ResourceDescriptorHeap[resources.multiple_scattering_texture];
  Texture2D<float4> irradiance_texture =
      ResourceDescriptorHeap[resources.irradiance_texture];
  RWTexture3D<float4> scattering_density =
      ResourceDescriptorHeap[resources.output0];
  Store(scattering_density, 0, texel, float4(
      ComputeScatteringDensityTexture(constants.atmosphere,
          transmittance_texture, single_rayleigh_scattering_texture,
          single_mie_scattering_texture, multiple_scattering_texture,
          irradiance_texture, lut_sampler, texel + 0.5,
          resources.scattering_order), 1.0));
}

/*
<p>The indirect irradiance shader computes the ground irradiance due to the
given scattering order, stores it in the delta irradiance texture, and
accumulates it, converted to luminance, in the irradiance texture:
*/

[RootSignature(PRECOMPUTE_ROOT_SIGNATURE)]
[numthreads(8, 8, 1)]
void ComputeIndirectIrradianceCS(uint3 texel : SV_DispatchThreadID) {
  if (any(texel.xy >= uint2(IRRADIANCE_TEXTURE_WIDTH,
                            IRRADIANCE_TEXTURE_HEIGHT))) {
    return;
  }
  Texture3D<float4> single_rayleigh_scattering_texture =
      ResourceDescriptorHeap[resources.single_rayleigh_scattering_texture];
  Texture3D<float4> single_mie_scattering_texture =
      ResourceDescriptorHeap[resources.single_mie_scattering_texture];
  Texture3D<float4> multiple_scattering_texture =
      ResourceDescriptorHeap[resources.multiple_scattering_texture];
  RWTexture2D<float4> delta_irradiance =
      ResourceDescriptorHeap[resources.output0];
  RWTexture2D<float4> irradiance = ResourceDescriptorHeap[resources.output1];
  float3 indirect_irradiance = ComputeIndirectIrradianceTexture(
      constants.atmosphere, single_rayleigh_scattering_texture,
      single_mie_scattering_texture, multiple_scattering_texture, lut_sampler,
      texel.xy + 0.5, resources.scattering_order);
  Store(delta_irradiance, 0, texel.xy, float4(indirect_irradiance, 1.0));
  Store(irradiance, 1, texel.xy, float4(
      mul(constants.luminance_from_radiance, indirect_irradiance), 0.0));
}

/*
<p>The multiple scattering shader computes the multiple scattering for
the current scattering order, from the scattering density computed above, stores
it in the delta multiple scattering texture, and accumulates it, converted to
luminance and divided by the Rayleigh phase function, in the scattering texture
(the phase function is applied at render time, see
<code>GetSkyRadiance</code>):
*/

[RootSignature(PRECOMPUTE_ROOT_SIGNATURE)]
[numthreads(8, 8, 1)]
void ComputeMultipleScatteringCS(uint3 texel : SV_DispatchThreadID) {
  if (any(texel >= uint3(SCATTERING_TEXTURE_WIDTH, SCATTERING_TEXTURE_HEIGHT,
                         SCATTERING_TEXTURE_DEPTH))) {
    return;
  }
  Texture2D<float4> transmittance_texture =
      ResourceDescriptorHeap[resources.transmittance_texture];
  Texture3D<float4> scattering_density_texture =
      ResourceDescriptorHeap[resources.scattering_density_texture];
  RWTexture3D<float4> delta_multiple_scattering =
      ResourceDescriptorHeap[resources.output0];
  RWTexture3D<float4> scattering = ResourceDescriptorHeap[resources.output1];
  float nu;
  float3 multiple_scattering = ComputeMultipleScatteringTexture(
      constants.atmosphere, transmittance_texture, scattering_density_texture,
      lut_sampler, texel + 0.5, nu);
  Store(delta_multiple_scattering, 0, texel, float4(multiple_scattering, 1.0));
  Store(scattering, 1, texel, float4(
      mul(constants.luminance_from_radiance, multiple_scattering) /
          RayleighPhaseFunction(nu),
      0.0));
}

/*
<p>With the <code>accumulate_in_float32</code> option, the scattering orders
(and the groups of 3 wavelengths) are accumulated in temporary single precision
textures, instead of directly in the final half precision textures (see
<code>Model::Init</code>), to avoid the accumulation of rounding errors. The
following shader then converts these sums to half precision, once, by
copying a single precision texture (whose shader resource view index is
provided in the <code>multiple_scattering_texture</code> root constant) into
the final half precision texture (<code>output0</code>):
*/

[RootSignature(PRECOMPUTE_ROOT_SIGNATURE)]
[numthreads(8, 8, 1)]
void ConvertToHalfCS(uint3 texel : SV_DispatchThreadID) {
  if (any(texel >= uint3(SCATTERING_TEXTURE_WIDTH, SCATTERING_TEXTURE_HEIGHT,
                         SCATTERING_TEXTURE_DEPTH))) {
    return;
  }
  Texture3D<float4> input =
      ResourceDescriptorHeap[resources.multiple_scattering_texture];
  RWTexture3D<float4> output = ResourceDescriptorHeap[resources.output0];
  output[texel] = input[texel];
}
