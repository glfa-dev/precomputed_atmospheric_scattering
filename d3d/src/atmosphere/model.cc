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

/*<h2>src/atmosphere/model.cc</h2>

<p>This file implements the <a href="model.h.html">API of our atmosphere
model</a>, with Direct3D 12. It is a port of the OpenGL
<code>atmosphere/model.cc</code> file. Its main role is to precompute the
transmittance, scattering and irradiance textures. The HLSL functions to
precompute them are provided in <a href=
"../../shaders/atmosphere/functions.hlsli.html">functions.hlsli</a>, and are
wrapped in compute shaders in <a href=
"../../shaders/atmosphere/precompute.hlsl.html">precompute.hlsl</a>. These
shaders must be called in the correct order, with the correct input and output
textures, to precompute each scattering order in sequence, as described in
Algorithm 4.1 of <a href="https://hal.inria.fr/inria-00288758/en">our paper</a>.
This is the role of the following C++ code.
*/

#include "atmosphere/model.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "atmosphere/constants.h"

/*
<p>The rest of this file is organized in 2 parts:
<ul>
<li>the <a href="#utilities">first part</a> provides utility functions, to
compute the radiance to luminance conversion factors,</li>
<li>the <a href="#implementation">second part</a> provides the actual
implementation of the <code>Model</code> class, using these utilities.</li>
</ul>

<h3 id="utilities">Utility functions</h3>

<p>We first need a function to compute the value of the conversion
constants *<code>_RADIANCE_TO_LUMINANCE</code>, used to convert the
spectral results into luminance values. These are the constants k_r, k_g, k_b
described in Section 14.3 of <a href="https://arxiv.org/pdf/1612.04336.pdf">A
Qualitative and Quantitative Evaluation of 8 Clear Sky Models</a>.

<p>Computing their value requires an integral of a function times a CIE color
matching function. Thus, we first need functions to interpolate an arbitrary
function (specified by some samples), and a CIE color matching function
(specified by tabulated values), at an arbitrary wavelength. This is the purpose
of the following two functions (identical to the OpenGL version):
*/

namespace atmosphere {

namespace {

constexpr int kLambdaMin = 360;
constexpr int kLambdaMax = 830;

double CieColorMatchingFunctionTableValue(double wavelength, int column) {
  if (wavelength <= kLambdaMin || wavelength >= kLambdaMax) {
    return 0.0;
  }
  double u = (wavelength - kLambdaMin) / 5.0;
  int row = static_cast<int>(std::floor(u));
  assert(row >= 0 && row + 1 < 95);
  assert(CIE_2_DEG_COLOR_MATCHING_FUNCTIONS[4 * row] <= wavelength &&
         CIE_2_DEG_COLOR_MATCHING_FUNCTIONS[4 * (row + 1)] >= wavelength);
  u -= row;
  return CIE_2_DEG_COLOR_MATCHING_FUNCTIONS[4 * row + column] * (1.0 - u) +
      CIE_2_DEG_COLOR_MATCHING_FUNCTIONS[4 * (row + 1) + column] * u;
}

double Interpolate(
    const std::vector<double>& wavelengths,
    const std::vector<double>& wavelength_function,
    double wavelength) {
  assert(wavelength_function.size() == wavelengths.size());
  if (wavelength < wavelengths[0]) {
    return wavelength_function[0];
  }
  for (unsigned int i = 0; i < wavelengths.size() - 1; ++i) {
    if (wavelength < wavelengths[i + 1]) {
      double u =
          (wavelength - wavelengths[i]) / (wavelengths[i + 1] - wavelengths[i]);
      return
          wavelength_function[i] * (1.0 - u) + wavelength_function[i + 1] * u;
    }
  }
  return wavelength_function[wavelength_function.size() - 1];
}

/*
<p>We can then implement a utility function to compute the "spectral radiance to
luminance" conversion constants (see Section 14.3 in <a
href="https://arxiv.org/pdf/1612.04336.pdf">A Qualitative and Quantitative
Evaluation of 8 Clear Sky Models</a> for their definitions):
*/

// The returned constants are in lumen.nm / watt.
void ComputeSpectralRadianceToLuminanceFactors(
    const std::vector<double>& wavelengths,
    const std::vector<double>& solar_irradiance,
    double lambda_power, double* k_r, double* k_g, double* k_b) {
  *k_r = 0.0;
  *k_g = 0.0;
  *k_b = 0.0;
  double solar_r = Interpolate(wavelengths, solar_irradiance, Model::kLambdaR);
  double solar_g = Interpolate(wavelengths, solar_irradiance, Model::kLambdaG);
  double solar_b = Interpolate(wavelengths, solar_irradiance, Model::kLambdaB);
  int dlambda = 1;
  for (int lambda = kLambdaMin; lambda < kLambdaMax; lambda += dlambda) {
    double x_bar = CieColorMatchingFunctionTableValue(lambda, 1);
    double y_bar = CieColorMatchingFunctionTableValue(lambda, 2);
    double z_bar = CieColorMatchingFunctionTableValue(lambda, 3);
    const double* xyz2srgb = XYZ_TO_SRGB;
    double r_bar =
        xyz2srgb[0] * x_bar + xyz2srgb[1] * y_bar + xyz2srgb[2] * z_bar;
    double g_bar =
        xyz2srgb[3] * x_bar + xyz2srgb[4] * y_bar + xyz2srgb[5] * z_bar;
    double b_bar =
        xyz2srgb[6] * x_bar + xyz2srgb[7] * y_bar + xyz2srgb[8] * z_bar;
    double irradiance = Interpolate(wavelengths, solar_irradiance, lambda);
    *k_r += r_bar * irradiance / solar_r *
        pow(lambda / Model::kLambdaR, lambda_power);
    *k_g += g_bar * irradiance / solar_g *
        pow(lambda / Model::kLambdaG, lambda_power);
    *k_b += b_bar * irradiance / solar_b *
        pow(lambda / Model::kLambdaB, lambda_power);
  }
  *k_r *= MAX_LUMINOUS_EFFICACY * dlambda;
  *k_g *= MAX_LUMINOUS_EFFICACY * dlambda;
  *k_b *= MAX_LUMINOUS_EFFICACY * dlambda;
}

/*
<p>We also need the C++ mirrors of the constant buffer and of the root constants
of the precomputation shaders (see <a href=
"../../shaders/atmosphere/precompute.hlsl.html">precompute.hlsl</a>). Note that
the <code>luminance_from_radiance</code> matrix is stored row by row, each row
being padded to 16 bytes:
*/

struct PrecomputeConstants {
  hlsl::AtmosphereParameters atmosphere;
  float luminance_from_radiance[3][4];
};
static_assert(offsetof(PrecomputeConstants, luminance_from_radiance) == 256);

struct PrecomputeResources {
  uint32_t transmittance_texture;
  uint32_t single_rayleigh_scattering_texture;
  uint32_t single_mie_scattering_texture;
  uint32_t multiple_scattering_texture;
  uint32_t irradiance_texture;
  uint32_t scattering_density_texture;
  uint32_t output0;
  uint32_t output1;
  uint32_t output2;
  uint32_t output3;
  int32_t scattering_order;
  uint32_t blend;
};
static_assert(sizeof(PrecomputeResources) == 12 * 4);

// Constant buffer views must be aligned to 256 bytes.
constexpr size_t kConstantBufferAlignment =
    D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
constexpr size_t kConstantsSize =
    (sizeof(PrecomputeConstants) + kConstantBufferAlignment - 1) &
    ~(kConstantBufferAlignment - 1);

// The thread group size of the precomputation shaders, in X and Y.
constexpr uint32_t kGroupSize = 8;

uint32_t GroupCount(uint32_t size) {
  return (size + kGroupSize - 1) / kGroupSize;
}

// The texture formats of the precomputed textures. 16F precision for the
// transmittance gives artifacts, so the 2D textures always use 32F precision.
constexpr DXGI_FORMAT kTexture2dFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;

DXGI_FORMAT Texture3dFormat(bool half_precision) {
  return half_precision ? DXGI_FORMAT_R16G16B16A16_FLOAT
                        : DXGI_FORMAT_R32G32B32A32_FLOAT;
}

}  // anonymous namespace

namespace hlsl {

std::vector<d3d12::ConstantBufferMember> GetAtmosphereParametersLayout(
    const std::string& name, size_t offset) {
  const std::string prefix = name + ".";
  return {
      {name, offset},
#define MEMBER(member) \
      {prefix + #member, offset + offsetof(AtmosphereParameters, member)}
      MEMBER(solar_irradiance),
      MEMBER(sun_angular_radius),
      MEMBER(bottom_radius),
      MEMBER(top_radius),
      MEMBER(rayleigh_density),
      {prefix + "rayleigh_density.layers.constant_term",
       offset + offsetof(AtmosphereParameters, rayleigh_density) +
           offsetof(DensityProfileLayer, constant_term)},
      MEMBER(rayleigh_scattering),
      MEMBER(mie_density),
      MEMBER(mie_scattering),
      MEMBER(mie_extinction),
      MEMBER(mie_phase_function_g),
      MEMBER(absorption_density),
      MEMBER(absorption_extinction),
      MEMBER(ground_albedo),
      MEMBER(mu_s_min),
#undef MEMBER
  };
}

}  // namespace hlsl

/*<h3 id="implementation">Model implementation</h3>

<p>Using the above utility functions, we can now implement the constructor of
the <code>Model</code> class. In the OpenGL version, this constructor generates
a piece of GLSL code that defines an <code>ATMOSPHERE</code> constant
containing the atmosphere parameters. Here, it creates instead a function that
returns an <code>AtmosphereParameters</code> struct, to be stored in a constant
buffer, for any 3 wavelengths. It also allocates the precomputed textures (but
does not initialize them).
*/

Model::Model(
    d3d12::Device* device,
    d3d12::DescriptorHeap* descriptor_heap,
    const d3d12::ShaderCompiler* shader_compiler,
    const std::vector<double>& wavelengths,
    const std::vector<double>& solar_irradiance,
    const double sun_angular_radius,
    double bottom_radius,
    double top_radius,
    const std::vector<DensityProfileLayer>& rayleigh_density,
    const std::vector<double>& rayleigh_scattering,
    const std::vector<DensityProfileLayer>& mie_density,
    const std::vector<double>& mie_scattering,
    const std::vector<double>& mie_extinction,
    double mie_phase_function_g,
    const std::vector<DensityProfileLayer>& absorption_density,
    const std::vector<double>& absorption_extinction,
    const std::vector<double>& ground_albedo,
    double max_sun_zenith_angle,
    double length_unit_in_meters,
    unsigned int num_precomputed_wavelengths,
    bool combine_scattering_textures,
    bool half_precision) :
        device_(device),
        descriptor_heap_(descriptor_heap),
        shader_compiler_(shader_compiler),
        num_precomputed_wavelengths_(num_precomputed_wavelengths),
        combine_scattering_textures_(combine_scattering_textures),
        half_precision_(half_precision) {
  // Note: the wavelengths are captured by value, because this lambda is used
  // by atmosphere_parameters_factory_, which can be called after the
  // constructor's arguments have been destroyed (at each Init call).
  auto to_float3 = [wavelengths](const std::vector<double>& v,
      const vec3& lambdas, double scale) {
    double r = Interpolate(wavelengths, v, lambdas[0]) * scale;
    double g = Interpolate(wavelengths, v, lambdas[1]) * scale;
    double b = Interpolate(wavelengths, v, lambdas[2]) * scale;
    return hlsl::float3{static_cast<float>(r), static_cast<float>(g),
                        static_cast<float>(b)};
  };
  auto density_layer =
      [length_unit_in_meters](const DensityProfileLayer& layer) {
        return hlsl::DensityProfileLayer{
            static_cast<float>(layer.width / length_unit_in_meters),
            static_cast<float>(layer.exp_term),
            static_cast<float>(layer.exp_scale * length_unit_in_meters),
            static_cast<float>(layer.linear_term * length_unit_in_meters),
            static_cast<float>(layer.constant_term)};
      };
  auto density_profile =
      [density_layer](std::vector<DensityProfileLayer> layers) {
        constexpr int kLayerCount = 2;
        while (layers.size() < kLayerCount) {
          layers.insert(layers.begin(), DensityProfileLayer());
        }
        hlsl::DensityProfile result = {};
        result.layer0 = density_layer(layers[0]);
        result.layer1 = density_layer(layers[1]);
        return result;
      };

  // Compute the values for the SKY_RADIANCE_TO_LUMINANCE constant. In theory
  // this should be 1 in precomputed illuminance mode (because the precomputed
  // textures already contain illuminance values). In practice, however, storing
  // true illuminance values in half precision textures yields artefacts
  // (because the values are too large), so we store illuminance values divided
  // by MAX_LUMINOUS_EFFICACY instead. This is why, in precomputed illuminance
  // mode, we set SKY_RADIANCE_TO_LUMINANCE to MAX_LUMINOUS_EFFICACY.
  bool precompute_illuminance = num_precomputed_wavelengths > 3;
  double sky_k_r, sky_k_g, sky_k_b;
  if (precompute_illuminance) {
    sky_k_r = sky_k_g = sky_k_b = MAX_LUMINOUS_EFFICACY;
  } else {
    ComputeSpectralRadianceToLuminanceFactors(wavelengths, solar_irradiance,
        -3 /* lambda_power */, &sky_k_r, &sky_k_g, &sky_k_b);
  }
  // Compute the values for the SUN_RADIANCE_TO_LUMINANCE constant.
  double sun_k_r, sun_k_g, sun_k_b;
  ComputeSpectralRadianceToLuminanceFactors(wavelengths, solar_irradiance,
      0 /* lambda_power */, &sun_k_r, &sun_k_g, &sun_k_b);
  sky_spectral_radiance_to_luminance_ = {static_cast<float>(sky_k_r),
      static_cast<float>(sky_k_g), static_cast<float>(sky_k_b)};
  sun_spectral_radiance_to_luminance_ = {static_cast<float>(sun_k_r),
      static_cast<float>(sun_k_g), static_cast<float>(sun_k_b)};

  // A function that creates the atmosphere parameters, specialized for the 3
  // wavelengths in 'lambdas'.
  atmosphere_parameters_factory_ = [=](const vec3& lambdas) {
    hlsl::AtmosphereParameters parameters = {};
    parameters.solar_irradiance = to_float3(solar_irradiance, lambdas, 1.0);
    parameters.sun_angular_radius = static_cast<float>(sun_angular_radius);
    parameters.bottom_radius =
        static_cast<float>(bottom_radius / length_unit_in_meters);
    parameters.top_radius =
        static_cast<float>(top_radius / length_unit_in_meters);
    parameters.rayleigh_density = density_profile(rayleigh_density);
    parameters.rayleigh_scattering =
        to_float3(rayleigh_scattering, lambdas, length_unit_in_meters);
    parameters.mie_density = density_profile(mie_density);
    parameters.mie_scattering =
        to_float3(mie_scattering, lambdas, length_unit_in_meters);
    parameters.mie_extinction =
        to_float3(mie_extinction, lambdas, length_unit_in_meters);
    parameters.mie_phase_function_g = static_cast<float>(mie_phase_function_g);
    parameters.absorption_density = density_profile(absorption_density);
    parameters.absorption_extinction =
        to_float3(absorption_extinction, lambdas, length_unit_in_meters);
    parameters.ground_albedo = to_float3(ground_albedo, lambdas, 1.0);
    parameters.mu_s_min = static_cast<float>(cos(max_sun_zenith_angle));
    return parameters;
  };
  atmosphere_parameters_ =
      atmosphere_parameters_factory_({kLambdaR, kLambdaG, kLambdaB});

  // Allocate the precomputed textures, but don't precompute them yet. Unlike
  // in OpenGL, RGB formats can't be used for unordered access views, so we
  // always use RGBA textures.
  ID3D12Device* d3d_device = device_->device();
  transmittance_texture_ = d3d12::Texture::Create2d(
      d3d_device, descriptor_heap_, TRANSMITTANCE_TEXTURE_WIDTH,
      TRANSMITTANCE_TEXTURE_HEIGHT, kTexture2dFormat, true,
      L"Transmittance texture");
  scattering_texture_ = d3d12::Texture::Create3d(
      d3d_device, descriptor_heap_, SCATTERING_TEXTURE_WIDTH,
      SCATTERING_TEXTURE_HEIGHT, SCATTERING_TEXTURE_DEPTH,
      Texture3dFormat(half_precision), true, L"Scattering texture");
  if (!combine_scattering_textures) {
    optional_single_mie_scattering_texture_ = d3d12::Texture::Create3d(
        d3d_device, descriptor_heap_, SCATTERING_TEXTURE_WIDTH,
        SCATTERING_TEXTURE_HEIGHT, SCATTERING_TEXTURE_DEPTH,
        Texture3dFormat(half_precision), true,
        L"Single Mie scattering texture");
  }
  irradiance_texture_ = d3d12::Texture::Create2d(
      d3d_device, descriptor_heap_, IRRADIANCE_TEXTURE_WIDTH,
      IRRADIANCE_TEXTURE_HEIGHT, kTexture2dFormat, true, L"Irradiance texture");
}

/*
<p>The destructor is trivial, since the textures release their resources and
descriptors automatically:
*/

Model::~Model() = default;

/*
<p>The preprocessor macros needed by the shaders including our HLSL files are
the texture sizes (from <a href="constants.h.html">constants.h</a>), and the
macros corresponding to the options of this model:
*/

std::vector<d3d12::ShaderDefine> Model::shader_defines() const {
  std::vector<d3d12::ShaderDefine> defines = {
      {"TRANSMITTANCE_TEXTURE_WIDTH",
       std::to_string(TRANSMITTANCE_TEXTURE_WIDTH)},
      {"TRANSMITTANCE_TEXTURE_HEIGHT",
       std::to_string(TRANSMITTANCE_TEXTURE_HEIGHT)},
      {"SCATTERING_TEXTURE_R_SIZE", std::to_string(SCATTERING_TEXTURE_R_SIZE)},
      {"SCATTERING_TEXTURE_MU_SIZE",
       std::to_string(SCATTERING_TEXTURE_MU_SIZE)},
      {"SCATTERING_TEXTURE_MU_S_SIZE",
       std::to_string(SCATTERING_TEXTURE_MU_S_SIZE)},
      {"SCATTERING_TEXTURE_NU_SIZE",
       std::to_string(SCATTERING_TEXTURE_NU_SIZE)},
      {"IRRADIANCE_TEXTURE_WIDTH", std::to_string(IRRADIANCE_TEXTURE_WIDTH)},
      {"IRRADIANCE_TEXTURE_HEIGHT", std::to_string(IRRADIANCE_TEXTURE_HEIGHT)},
  };
  if (combine_scattering_textures_) {
    defines.push_back({"COMBINED_SCATTERING_TEXTURES", ""});
  }
  if (radiance_api_enabled()) {
    defines.push_back({"RADIANCE_API_ENABLED", ""});
  }
  return defines;
}

/*
<p>The precomputations require a compute pipeline state for each entry point of
<a href="../../shaders/atmosphere/precompute.hlsl.html">precompute.hlsl</a>,
all using the same root signature (declared in the shader file). They are
compiled at each <code>Init</code> call (which makes it possible to reload the
shaders when their source code changes):
*/

struct Model::Programs {
  d3d12::ComPtr<ID3D12RootSignature> root_signature;
  d3d12::ComPtr<ID3D12PipelineState> compute_transmittance;
  d3d12::ComPtr<ID3D12PipelineState> compute_direct_irradiance;
  d3d12::ComPtr<ID3D12PipelineState> compute_single_scattering;
  d3d12::ComPtr<ID3D12PipelineState> compute_scattering_density;
  d3d12::ComPtr<ID3D12PipelineState> compute_indirect_irradiance;
  d3d12::ComPtr<ID3D12PipelineState> compute_multiple_scattering;
};

/*
<p>The precomputations also require temporary textures, in particular to store
the contribution of one scattering order, which is needed to compute the next
order of scattering (the final precomputed textures store the sum of all the
scattering orders):
*/

struct Model::TemporaryTextures {
  d3d12::Texture delta_irradiance;
  d3d12::Texture delta_rayleigh_scattering;
  d3d12::Texture delta_mie_scattering;
  d3d12::Texture delta_scattering_density;
  // delta_multiple_scattering is only needed to compute scattering order 3 or
  // more, while delta_rayleigh_scattering and delta_mie_scattering are only
  // needed to compute double scattering. Therefore, to save memory, we can
  // store delta_rayleigh_scattering and delta_multiple_scattering in the same
  // GPU texture.
  d3d12::Texture& delta_multiple_scattering() {
    return delta_rayleigh_scattering;
  }
};

/*
<p>The <code>Init</code> method compiles these programs, allocates the
temporary resources, records the precomputation commands, executes them, and
waits for their completion. As in the OpenGL version, the actual precomputations
depend on whether we want to store precomputed irradiance or illuminance values:
<ul>
<li>In precomputed irradiance mode, we simply need to call
<code>Precompute</code> once, with the parameters for the
<code>kLambdaR</code>, <code>kLambdaG</code>, <code>kLambdaB</code>
wavelengths, and with an identity luminance_from_radiance matrix.</li>
<li>In precomputed illuminance mode, we need to precompute the scattering for
<code>num_precomputed_wavelengths</code> wavelengths, 3 at a time, each time
converting the results to luminance (with the
<code>luminance_from_radiance</code> matrix), and accumulating them in the
final textures (via the <code>blend</code> argument).</li>
</ul>
Since the atmosphere parameters and the luminance_from_radiance matrix are
stored in a constant buffer, instead of being compiled in the shaders, each
group of 3 wavelengths simply uses a different part of an upload buffer:
*/

void Model::Init(unsigned int num_scattering_orders) {
  ID3D12Device* d3d_device = device_->device();

  // Compile the shaders first, so that the precomputed textures are left
  // unchanged if this fails.
  const std::vector<d3d12::ShaderDefine> defines = shader_defines();
  auto compile = [&](const char* entry_point) {
    return shader_compiler_->Compile("atmosphere/precompute.hlsl", entry_point,
                                     "cs_6_6", defines);
  };
  const d3d12::Shader transmittance_shader = compile("ComputeTransmittanceCS");
  const d3d12::Shader shaders[] = {
    transmittance_shader,
    compile("ComputeDirectIrradianceCS"),
    compile("ComputeSingleScatteringCS"),
    compile("ComputeScatteringDensityCS"),
    compile("ComputeIndirectIrradianceCS"),
    compile("ComputeMultipleScatteringCS"),
  };
#ifndef NDEBUG
  // Check that the C++ and HLSL constant buffer layouts are the same.
  auto members = hlsl::GetAtmosphereParametersLayout("constants.atmosphere", 0);
  members.push_back({"constants.luminance_from_radiance",
                     offsetof(PrecomputeConstants, luminance_from_radiance)});
  d3d12::ValidateConstantBufferLayout(shaders[2], "constants", members,
                                      sizeof(PrecomputeConstants));
#endif
  Programs programs;
  programs.root_signature =
      d3d12::CreateRootSignature(d3d_device, transmittance_shader);
  d3d12::ComPtr<ID3D12PipelineState>* pipeline_states[] = {
    &programs.compute_transmittance,
    &programs.compute_direct_irradiance,
    &programs.compute_single_scattering,
    &programs.compute_scattering_density,
    &programs.compute_indirect_irradiance,
    &programs.compute_multiple_scattering,
  };
  for (int i = 0; i < 6; ++i) {
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = programs.root_signature.Get();
    desc.CS = shaders[i].GetBytecode();
    D3D12_CHECK(d3d_device->CreateComputePipelineState(
        &desc, IID_PPV_ARGS(pipeline_states[i]->GetAddressOf())));
  }

  // Allocate the temporary textures.
  TemporaryTextures textures;
  const DXGI_FORMAT format_3d = Texture3dFormat(half_precision_);
  textures.delta_irradiance = d3d12::Texture::Create2d(
      d3d_device, descriptor_heap_, IRRADIANCE_TEXTURE_WIDTH,
      IRRADIANCE_TEXTURE_HEIGHT, kTexture2dFormat, true,
      L"Delta irradiance texture");
  textures.delta_rayleigh_scattering = d3d12::Texture::Create3d(
      d3d_device, descriptor_heap_, SCATTERING_TEXTURE_WIDTH,
      SCATTERING_TEXTURE_HEIGHT, SCATTERING_TEXTURE_DEPTH, format_3d, true,
      L"Delta Rayleigh scattering texture");
  textures.delta_mie_scattering = d3d12::Texture::Create3d(
      d3d_device, descriptor_heap_, SCATTERING_TEXTURE_WIDTH,
      SCATTERING_TEXTURE_HEIGHT, SCATTERING_TEXTURE_DEPTH, format_3d, true,
      L"Delta Mie scattering texture");
  textures.delta_scattering_density = d3d12::Texture::Create3d(
      d3d_device, descriptor_heap_, SCATTERING_TEXTURE_WIDTH,
      SCATTERING_TEXTURE_HEIGHT, SCATTERING_TEXTURE_DEPTH, format_3d, true,
      L"Delta scattering density texture");

  // Allocate an upload buffer for the constants of each group of 3 wavelengths
  // (plus one for the final transmittance pass in precomputed illuminance
  // mode), as well as a command list and a GPU timer.
  const unsigned int num_iterations = num_precomputed_wavelengths_ <= 3
      ? 1 : (num_precomputed_wavelengths_ + 2) / 3;
  d3d12::ComPtr<ID3D12Resource> constants_buffer;
  CD3DX12_HEAP_PROPERTIES upload_heap(D3D12_HEAP_TYPE_UPLOAD);
  auto buffer_desc =
      CD3DX12_RESOURCE_DESC::Buffer(kConstantsSize * (num_iterations + 1));
  D3D12_CHECK(d3d_device->CreateCommittedResource(
      &upload_heap, D3D12_HEAP_FLAG_NONE, &buffer_desc,
      D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
      IID_PPV_ARGS(&constants_buffer)));
  uint8_t* constants_data;
  D3D12_CHECK(constants_buffer->Map(
      0, nullptr, reinterpret_cast<void**>(&constants_data)));
  auto set_constants = [&](unsigned int index, const vec3& lambdas,
                           const mat3& luminance_from_radiance) {
    PrecomputeConstants constants = {};
    constants.atmosphere = atmosphere_parameters_factory_(lambdas);
    for (int row = 0; row < 3; ++row) {
      for (int column = 0; column < 3; ++column) {
        constants.luminance_from_radiance[row][column] =
            luminance_from_radiance[3 * row + column];
      }
    }
    memcpy(constants_data + index * kConstantsSize, &constants,
           sizeof(constants));
    return constants_buffer->GetGPUVirtualAddress() + index * kConstantsSize;
  };

  d3d12::ComPtr<ID3D12CommandAllocator> allocator;
  d3d12::ComPtr<ID3D12GraphicsCommandList> command_list;
  D3D12_CHECK(d3d_device->CreateCommandAllocator(
      D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  D3D12_CHECK(d3d_device->CreateCommandList(
      0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
      IID_PPV_ARGS(&command_list)));
  command_list->SetName(L"Atmosphere precomputation");
  d3d12::GpuTimer timer(d3d_device, device_->queue(),
                        num_iterations * 3 * num_scattering_orders + 1);

  // The descriptor heap must be set before the root signature, since the root
  // signature uses the CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED flag.
  ID3D12DescriptorHeap* heaps[] = {descriptor_heap_->heap()};
  command_list->SetDescriptorHeaps(1, heaps);
  command_list->SetComputeRootSignature(programs.root_signature.Get());

  if (num_precomputed_wavelengths_ <= 3) {
    vec3 lambdas{kLambdaR, kLambdaG, kLambdaB};
    mat3 luminance_from_radiance{1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
    Precompute(command_list.Get(), &timer, programs, textures,
        set_constants(0, lambdas, luminance_from_radiance),
        false /* blend */, num_scattering_orders);
  } else {
    // Note: kLambdaMin and kLambdaMax are 360 and 830 nm, as in the OpenGL
    // version.
    double dlambda =
        static_cast<double>(kLambdaMax - kLambdaMin) / (3 * num_iterations);
    for (unsigned int i = 0; i < num_iterations; ++i) {
      vec3 lambdas{
        kLambdaMin + (3 * i + 0.5) * dlambda,
        kLambdaMin + (3 * i + 1.5) * dlambda,
        kLambdaMin + (3 * i + 2.5) * dlambda
      };
      auto coeff = [dlambda](double lambda, int component) {
        // Note that we don't include MAX_LUMINOUS_EFFICACY here, to avoid
        // artefacts due to too large values when using half precision on GPU.
        // We add this term back in the luminance API functions, via
        // sky_spectral_radiance_to_luminance (see also the comments in the
        // Model constructor).
        double x = CieColorMatchingFunctionTableValue(lambda, 1);
        double y = CieColorMatchingFunctionTableValue(lambda, 2);
        double z = CieColorMatchingFunctionTableValue(lambda, 3);
        return static_cast<float>((
            XYZ_TO_SRGB[component * 3] * x +
            XYZ_TO_SRGB[component * 3 + 1] * y +
            XYZ_TO_SRGB[component * 3 + 2] * z) * dlambda);
      };
      mat3 luminance_from_radiance{
        coeff(lambdas[0], 0), coeff(lambdas[1], 0), coeff(lambdas[2], 0),
        coeff(lambdas[0], 1), coeff(lambdas[1], 1), coeff(lambdas[2], 1),
        coeff(lambdas[0], 2), coeff(lambdas[1], 2), coeff(lambdas[2], 2)
      };
      Precompute(command_list.Get(), &timer, programs, textures,
          set_constants(i, lambdas, luminance_from_radiance),
          i > 0 /* blend */, num_scattering_orders);
    }

    // After the above iterations, the transmittance texture contains the
    // transmittance for the 3 wavelengths used at the last iteration. But we
    // want the transmittance at kLambdaR, kLambdaG, kLambdaB instead, so we
    // must recompute it here for these 3 wavelengths:
    mat3 identity{1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
    command_list->SetComputeRootConstantBufferView(0, set_constants(
        num_iterations, {kLambdaR, kLambdaG, kLambdaB}, identity));
    d3d12::Transition(command_list.Get(), {
        {&transmittance_texture_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS}});
    PrecomputeResources resources = {};
    resources.output0 = transmittance_texture_.uav();
    timer.Begin(command_list.Get(), "Transmittance");
    command_list->SetPipelineState(programs.compute_transmittance.Get());
    command_list->SetComputeRoot32BitConstants(1, 12, &resources, 0);
    command_list->Dispatch(GroupCount(TRANSMITTANCE_TEXTURE_WIDTH),
                           GroupCount(TRANSMITTANCE_TEXTURE_HEIGHT), 1);
    timer.End(command_list.Get());
  }

  // Leave the precomputed textures in a state where they can be read by any
  // shader, execute the commands, and wait for their completion (the temporary
  // resources allocated in this method are then released automatically).
  d3d12::Transition(command_list.Get(), {
      {&transmittance_texture_, d3d12::Texture::kShaderResourceState},
      {&scattering_texture_, d3d12::Texture::kShaderResourceState},
      {&irradiance_texture_, d3d12::Texture::kShaderResourceState}});
  if (!combine_scattering_textures_) {
    d3d12::Transition(command_list.Get(),
        {{&optional_single_mie_scattering_texture_,
          d3d12::Texture::kShaderResourceState}});
  }
  timer.Resolve(command_list.Get());
  device_->ExecuteAndWait(command_list.Get());
  constants_buffer->Unmap(0, nullptr);
  precompute_timings_ = timer.ReadResults();
}

/*
<p>The utility method <code>ConvertSpectrumToLinearSrgb</code> is implemented
with a simple numerical integration of the given function, times the CIE color
matching funtions (with an integration step of 1nm), followed by a matrix
multiplication:
*/

void Model::ConvertSpectrumToLinearSrgb(
    const std::vector<double>& wavelengths,
    const std::vector<double>& spectrum,
    double* r, double* g, double* b) {
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
  const int dlambda = 1;
  for (int lambda = kLambdaMin; lambda < kLambdaMax; lambda += dlambda) {
    double value = Interpolate(wavelengths, spectrum, lambda);
    x += CieColorMatchingFunctionTableValue(lambda, 1) * value;
    y += CieColorMatchingFunctionTableValue(lambda, 2) * value;
    z += CieColorMatchingFunctionTableValue(lambda, 3) * value;
  }
  *r = MAX_LUMINOUS_EFFICACY *
      (XYZ_TO_SRGB[0] * x + XYZ_TO_SRGB[1] * y + XYZ_TO_SRGB[2] * z) * dlambda;
  *g = MAX_LUMINOUS_EFFICACY *
      (XYZ_TO_SRGB[3] * x + XYZ_TO_SRGB[4] * y + XYZ_TO_SRGB[5] * z) * dlambda;
  *b = MAX_LUMINOUS_EFFICACY *
      (XYZ_TO_SRGB[6] * x + XYZ_TO_SRGB[7] * y + XYZ_TO_SRGB[8] * z) * dlambda;
}

/*
<p>Finally, we provide the actual implementation of the precomputation algorithm
described in Algorithm 4.1 of
<a href="https://hal.inria.fr/inria-00288758/en">our paper</a>. Each step is
explained by the inline comments below. Compared to the OpenGL version, each
step is a compute shader dispatch instead of a quad rendering in a framebuffer
object, the "blending" of the results in the output textures is done in the
shaders (controlled by a bit mask, with one bit per output texture, set as in
the OpenGL version), and the textures must be transitioned between the shader
resource and unordered access states, depending on how they are used by each
step. Finally, since some textures are written by several consecutive steps (in
the unordered access state), we add a UAV barrier after each dispatch to make
sure that each step sees the results of the previous ones.
*/

void Model::Precompute(
    ID3D12GraphicsCommandList* command_list,
    d3d12::GpuTimer* timer,
    const Programs& programs,
    TemporaryTextures& textures,
    D3D12_GPU_VIRTUAL_ADDRESS constants,
    bool blend,
    unsigned int num_scattering_orders) {
  constexpr D3D12_RESOURCE_STATES kSrv = d3d12::Texture::kShaderResourceState;
  constexpr D3D12_RESOURCE_STATES kUav = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  auto dispatch = [&](const char* name, ID3D12PipelineState* pipeline_state,
                      const PrecomputeResources& resources, uint32_t width,
                      uint32_t height, uint32_t depth) {
    timer->Begin(command_list, name);
    command_list->SetPipelineState(pipeline_state);
    command_list->SetComputeRoot32BitConstants(
        1, sizeof(resources) / 4, &resources, 0);
    command_list->Dispatch(GroupCount(width), GroupCount(height), depth);
    timer->End(command_list);
    d3d12::UavBarrier(command_list);
  };
  command_list->SetComputeRootConstantBufferView(0, constants);

  // Compute the transmittance, and store it in transmittance_texture_.
  d3d12::Transition(command_list, {{&transmittance_texture_, kUav}});
  PrecomputeResources resources = {};
  resources.output0 = transmittance_texture_.uav();
  dispatch("Transmittance", programs.compute_transmittance.Get(), resources,
           TRANSMITTANCE_TEXTURE_WIDTH, TRANSMITTANCE_TEXTURE_HEIGHT, 1);

  // Compute the direct irradiance, store it in delta_irradiance_texture and,
  // depending on 'blend', either initialize irradiance_texture_ with zeros or
  // leave it unchanged (we don't want the direct irradiance in
  // irradiance_texture_, but only the irradiance from the sky).
  d3d12::Transition(command_list, {
      {&transmittance_texture_, kSrv},
      {&textures.delta_irradiance, kUav},
      {&irradiance_texture_, kUav}});
  resources = {};
  resources.transmittance_texture = transmittance_texture_.srv();
  resources.output0 = textures.delta_irradiance.uav();
  resources.output1 = irradiance_texture_.uav();
  resources.blend = blend ? 0b10 : 0;
  dispatch("Direct irradiance", programs.compute_direct_irradiance.Get(),
           resources, IRRADIANCE_TEXTURE_WIDTH, IRRADIANCE_TEXTURE_HEIGHT, 1);

  // Compute the rayleigh and mie single scattering, store them in
  // delta_rayleigh_scattering_texture and delta_mie_scattering_texture, and
  // either store them or accumulate them in scattering_texture_ and
  // optional_single_mie_scattering_texture_.
  d3d12::Transition(command_list, {
      {&textures.delta_rayleigh_scattering, kUav},
      {&textures.delta_mie_scattering, kUav},
      {&scattering_texture_, kUav}});
  resources = {};
  resources.transmittance_texture = transmittance_texture_.srv();
  resources.output0 = textures.delta_rayleigh_scattering.uav();
  resources.output1 = textures.delta_mie_scattering.uav();
  resources.output2 = scattering_texture_.uav();
  if (!combine_scattering_textures_) {
    d3d12::Transition(command_list,
        {{&optional_single_mie_scattering_texture_, kUav}});
    resources.output3 = optional_single_mie_scattering_texture_.uav();
  }
  resources.blend = blend ? 0b1100 : 0;
  dispatch("Single scattering", programs.compute_single_scattering.Get(),
           resources, SCATTERING_TEXTURE_WIDTH, SCATTERING_TEXTURE_HEIGHT,
           SCATTERING_TEXTURE_DEPTH);

  // Compute the 2nd, 3rd and 4th order of scattering, in sequence.
  for (unsigned int scattering_order = 2;
       scattering_order <= num_scattering_orders;
       ++scattering_order) {
    // Compute the scattering density, and store it in
    // delta_scattering_density_texture.
    d3d12::Transition(command_list, {
        {&textures.delta_rayleigh_scattering, kSrv},
        {&textures.delta_mie_scattering, kSrv},
        {&textures.delta_multiple_scattering(), kSrv},
        {&textures.delta_irradiance, kSrv},
        {&textures.delta_scattering_density, kUav}});
    resources = {};
    resources.transmittance_texture = transmittance_texture_.srv();
    resources.single_rayleigh_scattering_texture =
        textures.delta_rayleigh_scattering.srv();
    resources.single_mie_scattering_texture =
        textures.delta_mie_scattering.srv();
    resources.multiple_scattering_texture =
        textures.delta_multiple_scattering().srv();
    resources.irradiance_texture = textures.delta_irradiance.srv();
    resources.output0 = textures.delta_scattering_density.uav();
    resources.scattering_order = static_cast<int32_t>(scattering_order);
    dispatch("Scattering density", programs.compute_scattering_density.Get(),
             resources, SCATTERING_TEXTURE_WIDTH, SCATTERING_TEXTURE_HEIGHT,
             SCATTERING_TEXTURE_DEPTH);

    // Compute the indirect irradiance, store it in delta_irradiance_texture and
    // accumulate it in irradiance_texture_.
    d3d12::Transition(command_list, {{&textures.delta_irradiance, kUav}});
    resources = {};
    resources.single_rayleigh_scattering_texture =
        textures.delta_rayleigh_scattering.srv();
    resources.single_mie_scattering_texture =
        textures.delta_mie_scattering.srv();
    resources.multiple_scattering_texture =
        textures.delta_multiple_scattering().srv();
    resources.output0 = textures.delta_irradiance.uav();
    resources.output1 = irradiance_texture_.uav();
    resources.scattering_order = static_cast<int32_t>(scattering_order - 1);
    resources.blend = 0b10;
    dispatch("Indirect irradiance", programs.compute_indirect_irradiance.Get(),
             resources, IRRADIANCE_TEXTURE_WIDTH, IRRADIANCE_TEXTURE_HEIGHT, 1);

    // Compute the multiple scattering, store it in
    // delta_multiple_scattering_texture, and accumulate it in
    // scattering_texture_.
    d3d12::Transition(command_list, {
        {&textures.delta_scattering_density, kSrv},
        {&textures.delta_multiple_scattering(), kUav}});
    resources = {};
    resources.transmittance_texture = transmittance_texture_.srv();
    resources.scattering_density_texture =
        textures.delta_scattering_density.srv();
    resources.output0 = textures.delta_multiple_scattering().uav();
    resources.output1 = scattering_texture_.uav();
    resources.blend = 0b10;
    dispatch("Multiple scattering", programs.compute_multiple_scattering.Get(),
             resources, SCATTERING_TEXTURE_WIDTH, SCATTERING_TEXTURE_HEIGHT,
             SCATTERING_TEXTURE_DEPTH);
  }
}

}  // namespace atmosphere
