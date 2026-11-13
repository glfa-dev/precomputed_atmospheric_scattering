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

/*<h2>src/demo/demo.cc</h2>

<p>This file, together with the shader <a href=
"../../shaders/demo/demo.hlsl.html">demo.hlsl</a>, shows how the API provided in
<a href="../atmosphere/model.h.html">model.h</a> can be used in practice. It
implements the <code>Demo</code> class whose header is defined in
<a href="demo.h.html">demo.h</a>, and is a port of the OpenGL demo
(<code>atmosphere/demo/demo.cc</code>). Most of the following code is
independent of our atmosphere model. The parts which are related to it are the
<code>InitModel</code> method, and the setup of the constant buffer and of the
textures used by the scene rendering shaders.
*/

#include "demo/demo.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <exception>

#include "atmosphere/constants.h"

namespace atmosphere {
namespace demo {

/*
<p>Our demo application renders a simple scene made of a purely spherical planet
and a large sphere (of 1km radius) lying on it. This scene is displayed by
rendering a full screen triangle, with a pixel shader computing the color of
each pixel by "ray tracing" (which is very simple here because the scene
consists of only two spheres). The shaders are defined in
<a href="../../shaders/demo/demo.hlsl.html">demo.hlsl</a>. They use the
following constant buffer, which must match the HLSL <code>DemoConstants</code>
constant buffer (the matrices are stored row by row, and declared as
<code>row_major</code> in HLSL):
*/

namespace {

constexpr double kPi = 3.1415926;
constexpr double kLengthUnitInMeters = 1000.0;

struct DemoConstants {
  float model_from_view[16];
  float view_from_clip[16];
  float camera[3];
  float exposure;
  float white_point[3];
  float padding0;
  float earth_center[3];
  float padding1;
  float sun_direction[3];
  float padding2;
  float sun_size[2];
  float padding3[2];
  hlsl::float3 sky_spectral_radiance_to_luminance;
  float padding4;
  hlsl::float3 sun_spectral_radiance_to_luminance;
  float padding5;
  hlsl::AtmosphereParameters atmosphere;
};
static_assert(offsetof(DemoConstants, camera) == 128);
static_assert(offsetof(DemoConstants, earth_center) == 160);
static_assert(offsetof(DemoConstants, sun_size) == 192);
static_assert(offsetof(DemoConstants, atmosphere) == 240);
static_assert(sizeof(DemoConstants) == 496);

// The size of the constant buffer of each frame, aligned to 256 bytes.
constexpr size_t kConstantsSize =
    (sizeof(DemoConstants) + D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT -
     1) & ~size_t(D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT - 1);

// The root constants of the demo shaders, i.e. the descriptor heap indices of
// the precomputed textures.
struct DemoResources {
  uint32_t transmittance_texture;
  uint32_t scattering_texture;
  uint32_t single_mie_scattering_texture;
  uint32_t irradiance_texture;
};

// The root constants of the LUT viewer shader (see lut_view.hlsl).
struct LutViewParameters {
  uint32_t source_texture;
  uint32_t is_3d;
  uint32_t slice;
  uint32_t show_alpha;
  float scale;
  uint32_t output_texture;
  uint32_t width;
  uint32_t height;
};

constexpr DXGI_FORMAT kRenderTargetFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

}  // anonymous namespace

/*
<p>The class constructor is straightforward and completely independent of our
atmosphere model (which is initialized in the separate method
<code>InitModel</code>). It creates a constant buffer for each frame in flight
(persistently mapped in an upload heap), and the texture used to display the
precomputed textures in the LUT viewer:
*/

Demo::Demo(d3d12::Device* device, d3d12::DescriptorHeap* descriptor_heap,
           const std::filesystem::path& shader_directory,
           uint32_t num_frames_in_flight) :
    device_(device),
    descriptor_heap_(descriptor_heap),
    shader_compiler_(shader_directory),
    shader_watcher_(shader_directory),
    num_frames_in_flight_(num_frames_in_flight),
    use_luminance_(NONE),
    do_white_balance_(false),
    show_ui_(true),
    model_dirty_(false),
    settings_modified_(false),
    init_time_milliseconds_(0.0),
    white_point_{1.0f, 1.0f, 1.0f},
    model_uses_rgb_coefficients_(false),
    rgb_coefficients_{},
    constants_data_(nullptr),
    view_distance_meters_(9000.0),
    view_zenith_angle_radians_(1.47),
    view_azimuth_angle_radians_(-0.1),
    sun_zenith_angle_radians_(1.3),
    sun_azimuth_angle_radians_(2.9),
    exposure_(10.0),
    show_lut_viewer_(false),
    lut_index_(0),
    lut_slice_(0),
    lut_scale_(1.0f),
    lut_show_alpha_(false) {
  ID3D12Device* d3d_device = device_->device();
  CD3DX12_HEAP_PROPERTIES upload_heap(D3D12_HEAP_TYPE_UPLOAD);
  auto buffer_desc =
      CD3DX12_RESOURCE_DESC::Buffer(kConstantsSize * num_frames_in_flight_);
  D3D12_CHECK(d3d_device->CreateCommittedResource(
      &upload_heap, D3D12_HEAP_FLAG_NONE, &buffer_desc,
      D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
      IID_PPV_ARGS(&constants_buffer_)));
  constants_buffer_->SetName(L"Demo constants");
  D3D12_CHECK(constants_buffer_->Map(
      0, nullptr, reinterpret_cast<void**>(&constants_data_)));

  lut_view_texture_ = d3d12::Texture::Create2d(
      d3d_device, descriptor_heap_, SCATTERING_TEXTURE_WIDTH,
      SCATTERING_TEXTURE_HEIGHT, DXGI_FORMAT_R8G8B8A8_UNORM, true,
      L"LUT viewer texture");

  InitModel();
}

Demo::~Demo() {
  device_->WaitIdle();
  constants_buffer_->Unmap(0, nullptr);
}

/*
<p>The "real" initialization work, which is specific to our atmosphere model,
is done in the following method. It starts with the creation of an atmosphere
<code>Model</code> instance, with parameters computed from the current
settings (by default they correspond to the Earth atmosphere, as in the OpenGL
demo):
*/

void Demo::InitModel() {
  // Values from "Reference Solar Spectral Irradiance: ASTM G-173", ETR column
  // (see http://rredc.nrel.gov/solar/spectra/am1.5/ASTMG173/ASTMG173.html),
  // summed and averaged in each bin (e.g. the value for 360nm is the average
  // of the ASTM G-173 values for all wavelengths between 360 and 370nm).
  // Values in W.m^-2 per nanometer.
  constexpr int kLambdaMin = 360;
  constexpr int kLambdaMax = 830;
  constexpr double kSolarIrradiance[48] = {
    1.11776, 1.14259, 1.01249, 1.14716, 1.72765, 1.73054, 1.6887, 1.61253,
    1.91198, 2.03474, 2.02042, 2.02212, 1.93377, 1.95809, 1.91686, 1.8298,
    1.8685, 1.8931, 1.85149, 1.8504, 1.8341, 1.8345, 1.8147, 1.78158, 1.7533,
    1.6965, 1.68194, 1.64654, 1.6048, 1.52143, 1.55622, 1.5113, 1.474, 1.4482,
    1.41018, 1.36775, 1.34188, 1.31429, 1.28303, 1.26758, 1.2367, 1.2082,
    1.18737, 1.14683, 1.12362, 1.1058, 1.07124, 1.04992
  };
  // Values from http://www.iup.uni-bremen.de/gruppen/molspec/databases/
  // referencespectra/o3spectra2011/index.html for 233K, summed and averaged in
  // each bin (e.g. the value for 360nm is the average of the original values
  // for all wavelengths between 360 and 370nm). Values in m^2.
  constexpr double kOzoneCrossSection[48] = {
    1.18e-27, 2.182e-28, 2.818e-28, 6.636e-28, 1.527e-27, 2.763e-27, 5.52e-27,
    8.451e-27, 1.582e-26, 2.316e-26, 3.669e-26, 4.924e-26, 7.752e-26, 9.016e-26,
    1.48e-25, 1.602e-25, 2.139e-25, 2.755e-25, 3.091e-25, 3.5e-25, 4.266e-25,
    4.672e-25, 4.398e-25, 4.701e-25, 5.019e-25, 4.305e-25, 3.74e-25, 3.215e-25,
    2.662e-25, 2.238e-25, 1.852e-25, 1.473e-25, 1.209e-25, 9.423e-26, 7.455e-26,
    6.566e-26, 5.105e-26, 4.15e-26, 4.228e-26, 3.237e-26, 2.451e-26, 2.801e-26,
    2.534e-26, 1.624e-26, 1.465e-26, 2.078e-26, 1.383e-26, 7.105e-27
  };
  // From https://en.wikipedia.org/wiki/Dobson_unit, in molecules.m^-2.
  constexpr double kDobsonUnit = 2.687e20;
  // Maximum number density of ozone molecules, in m^-3 (computed so at to get
  // the desired amount of ozone - for this we divide the Dobson units by the
  // integral of the default ozone density profile, which is equal to 15km).
  const double max_ozone_number_density =
      settings_.ozone_dobson_units * kDobsonUnit / 15000.0;

  const AtmosphereSettings& s = settings_;
  std::vector<double> wavelengths;
  std::vector<double> solar_irradiance;
  std::vector<double> rayleigh_scattering;
  std::vector<double> mie_scattering;
  std::vector<double> mie_extinction;
  std::vector<double> absorption_extinction;
  std::vector<double> ground_albedo;
  for (int l = kLambdaMin; l <= kLambdaMax; l += 10) {
    double lambda = static_cast<double>(l) * 1e-3;  // micro-meters
    double mie = s.mie_extinction * pow(lambda, -s.mie_angstrom_alpha);
    wavelengths.push_back(l);
    if (s.use_constant_solar_spectrum) {
      solar_irradiance.push_back(s.constant_solar_irradiance);
    } else {
      solar_irradiance.push_back(kSolarIrradiance[(l - kLambdaMin) / 10]);
    }
    rayleigh_scattering.push_back(s.rayleigh * pow(lambda, -4));
    mie_scattering.push_back(mie * s.mie_single_scattering_albedo);
    mie_extinction.push_back(mie);
    absorption_extinction.push_back(s.use_ozone ?
        max_ozone_number_density * kOzoneCrossSection[(l - kLambdaMin) / 10] :
        0.0);
    ground_albedo.push_back(s.ground_albedo);
  }

/*
<p>By default the <code>Model</code> samples these wavelength dependent
parameters at <code>kLambdaR</code>, <code>kLambdaG</code> and
<code>kLambdaB</code> (680, 550 and 440nm) in radiance mode. This gives a
greenish image (the solar spectrum is larger at 550nm than at 680nm, and the
sRGB red channel is mostly sensitive around 600nm, not 680nm), and purplish
twilights (680nm is in the weak tail of the ozone Chappuis absorption band,
which peaks around 600nm). Optionally, we can instead replace each parameter
$\beta(\lambda)$ with its average over each linear sRGB (i.e. Rec.709) channel
$c$, weighted by the solar spectrum $S(\lambda)$ and by the sRGB color matching
function $\bar{c}(\lambda)$:
$$\beta_c=\frac{\int S(\lambda)\beta(\lambda)\bar{c}(\lambda)d\lambda}
{\int S(\lambda)\bar{c}(\lambda)d\lambda}$$
and the solar spectrum with its linear sRGB color $\int S\bar{c}d\lambda$
(scaled by $10^{-5}$, like the luminance values in the luminance modes, so that
the same exposure gives the same brightness in all modes). With these values, a single scattering
event under a direct sunlight gives exactly the right sRGB color. But this is
only an approximation for long optical paths, because the transmittance
$\exp(-\int\beta)$ is not linear in $\beta$ (for instance, as the path length
increases, the light which remains in each channel is concentrated at the
wavelengths where the extinction is the smallest, which reduces the effective
extinction coefficient of this channel). In practice, compared with the
precomputed luminance mode, this removes the green tint of the default sampling
in daylight and its purple tint at twilight, but gives slightly less red colors
when the Sun is low, for the cost of a radiance precomputation. These RGB
coefficients are passed to the
<code>Model</code> as "spectra" sampled at exactly <code>kLambdaB</code>,
<code>kLambdaG</code> and <code>kLambdaR</code>, so that the
<code>Model</code> uses them unchanged:
*/

  std::vector<double> model_wavelengths = wavelengths;
  std::vector<double> model_solar_irradiance = solar_irradiance;
  std::array<std::vector<double>*, 5> coefficients = {
      &rayleigh_scattering, &mie_scattering, &mie_extinction,
      &absorption_extinction, &ground_albedo};
  std::array<std::array<double, 3>, 6> rgb_coefficients = {};
  if (s.use_rgb_coefficients) {
    // Note: the RGB coefficients are only meaningful in radiance mode.
    use_luminance_ = NONE;
    auto to_srgb = [&wavelengths](const std::vector<double>& spectrum) {
      std::array<double, 3> rgb;
      Model::ConvertSpectrumToLinearSrgb(
          wavelengths, spectrum, &rgb[0], &rgb[1], &rgb[2]);
      return rgb;
    };
    const std::array<double, 3> solar_rgb = to_srgb(solar_irradiance);
    // Same scale as the luminance modes, whose exposure is multiplied by 1e-5
    // (see Render), so that the same exposure gives the same brightness.
    constexpr double kLuminanceScale = 1e-5;
    for (int c = 0; c < 3; ++c) {
      rgb_coefficients[0][c] = solar_rgb[c] * kLuminanceScale;
    }
    for (size_t i = 0; i < coefficients.size(); ++i) {
      std::vector<double> weighted(wavelengths.size());
      for (size_t j = 0; j < wavelengths.size(); ++j) {
        weighted[j] = solar_irradiance[j] * (*coefficients[i])[j];
      }
      const std::array<double, 3> weighted_rgb = to_srgb(weighted);
      for (int c = 0; c < 3; ++c) {
        // The signed sRGB color matching functions can yield slightly
        // negative values (e.g. for the ozone absorption in the blue channel).
        rgb_coefficients[i + 1][c] =
            std::max(weighted_rgb[c] / solar_rgb[c], 0.0);
      }
    }
    // The Model interpolates its input spectra at kLambdaR, kLambdaG and
    // kLambdaB, and requires increasing wavelengths, hence the B,G,R order.
    auto bgr = [](const std::array<double, 3>& rgb) {
      return std::vector<double>{rgb[2], rgb[1], rgb[0]};
    };
    model_wavelengths = {Model::kLambdaB, Model::kLambdaG, Model::kLambdaR};
    model_solar_irradiance = bgr(rgb_coefficients[0]);
    for (size_t i = 0; i < coefficients.size(); ++i) {
      *coefficients[i] = bgr(rgb_coefficients[i + 1]);
    }
  }

/*
<p>We can then create the <code>Model</code>:
*/

  // The new model is first created and initialized separately, so that the
  // current one can be kept if this fails (e.g. because of a shader
  // compilation error).
  auto start = std::chrono::steady_clock::now();
  std::unique_ptr<Model> model;
  try {
    model = std::make_unique<Model>(device_, descriptor_heap_,
        &shader_compiler_, model_wavelengths, model_solar_irradiance,
        s.sun_angular_radius, s.bottom_radius, s.top_radius,
        std::vector<DensityProfileLayer>(s.rayleigh_density.begin(),
                                         s.rayleigh_density.end()),
        rayleigh_scattering,
        std::vector<DensityProfileLayer>(s.mie_density.begin(),
                                         s.mie_density.end()),
        mie_scattering, mie_extinction, s.mie_phase_function_g,
        std::vector<DensityProfileLayer>(s.absorption_density.begin(),
                                         s.absorption_density.end()),
        absorption_extinction, ground_albedo,
        s.max_sun_zenith_angle_degrees / 180.0 * kPi, kLengthUnitInMeters,
        use_luminance_ == PRECOMPUTED ? s.num_precomputed_wavelengths : 3,
        s.use_combined_textures, s.use_half_precision);
    model->Init(s.num_scattering_orders);
  } catch (const std::exception& error) {
    error_message_ = error.what();
    return;
  }
  init_time_milliseconds_ = std::chrono::duration<double, std::milli>(
      std::chrono::steady_clock::now() - start).count();
  model_ = std::move(model);
  model_uses_rgb_coefficients_ = s.use_rgb_coefficients;
  rgb_coefficients_ = rgb_coefficients;
  // The white point is computed from the full solar spectrum, even with RGB
  // coefficients (whose solar irradiance is the sRGB color of this spectrum).
  wavelengths_ = std::move(wavelengths);
  solar_irradiance_ = std::move(solar_irradiance);
  UpdateWhitePoint();

/*
<p>Then, it compiles the shaders used to render our demo scene, with the
preprocessor macros corresponding to the model options:
*/

  try {
    BuildPipelines();
    error_message_.clear();
  } catch (const std::exception& error) {
    // The previous pipeline state is not compatible with the new model.
    pipeline_state_.Reset();
    error_message_ = error.what();
  }
}

/*
<p>The white point used for the final "tone mapping" is either (1,1,1), or the
color of the solar spectrum, if white balance is enabled:
*/

void Demo::UpdateWhitePoint() {
  double white_point_r = 1.0;
  double white_point_g = 1.0;
  double white_point_b = 1.0;
  if (do_white_balance_ && !wavelengths_.empty()) {
    Model::ConvertSpectrumToLinearSrgb(wavelengths_, solar_irradiance_,
        &white_point_r, &white_point_g, &white_point_b);
    double white_point = (white_point_r + white_point_g + white_point_b) / 3.0;
    white_point_r /= white_point;
    white_point_g /= white_point;
    white_point_b /= white_point;
  }
  white_point_[0] = static_cast<float>(white_point_r);
  white_point_[1] = static_cast<float>(white_point_g);
  white_point_[2] = static_cast<float>(white_point_b);
}

/*
<p>The scene rendering shaders must be compiled with the preprocessor macros
returned by <code>Model::shader_defines()</code>, plus our own options. The
root signature is created from the compiled shader, where it is declared. We
also compile the LUT viewer shader here:
*/

void Demo::BuildPipelines() {
  ID3D12Device* d3d_device = device_->device();
  std::vector<d3d12::ShaderDefine> defines = model_->shader_defines();
  if (use_luminance_ != NONE) {
    defines.push_back({"USE_LUMINANCE", ""});
  }
  defines.push_back(
      {"LENGTH_UNIT_IN_METERS", std::to_string(kLengthUnitInMeters)});
  d3d12::Shader vertex_shader =
      shader_compiler_.Compile("demo/demo.hlsl", "DemoVS", "vs_6_6", defines);
  d3d12::Shader pixel_shader =
      shader_compiler_.Compile("demo/demo.hlsl", "DemoPS", "ps_6_6", defines);
  d3d12::Shader lut_view_shader =
      shader_compiler_.Compile("demo/lut_view.hlsl", "LutViewCS", "cs_6_6");
#ifndef NDEBUG
  // Check that the C++ and HLSL constant buffer layouts are the same.
  std::vector<d3d12::ConstantBufferMember> members =
      hlsl::GetAtmosphereParametersLayout(
          "atmosphere", offsetof(DemoConstants, atmosphere));
#define MEMBER(member) {#member, offsetof(DemoConstants, member)}
  members.insert(members.end(), {
      MEMBER(model_from_view), MEMBER(view_from_clip), MEMBER(camera),
      MEMBER(exposure), MEMBER(white_point), MEMBER(earth_center),
      MEMBER(sun_direction), MEMBER(sun_size),
      MEMBER(sky_spectral_radiance_to_luminance),
      MEMBER(sun_spectral_radiance_to_luminance)});
#undef MEMBER
  d3d12::ValidateConstantBufferLayout(pixel_shader, "DemoConstants", members,
                                      sizeof(DemoConstants));
#endif

  auto root_signature = d3d12::CreateRootSignature(d3d_device, vertex_shader);
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
  desc.pRootSignature = root_signature.Get();
  desc.VS = vertex_shader.GetBytecode();
  desc.PS = pixel_shader.GetBytecode();
  desc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
  desc.SampleMask = UINT_MAX;
  desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
  desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  desc.DepthStencilState.DepthEnable = FALSE;
  desc.DepthStencilState.StencilEnable = FALSE;
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  desc.NumRenderTargets = 1;
  desc.RTVFormats[0] = kRenderTargetFormat;
  desc.SampleDesc.Count = 1;
  d3d12::ComPtr<ID3D12PipelineState> pipeline_state;
  D3D12_CHECK(d3d_device->CreateGraphicsPipelineState(
      &desc, IID_PPV_ARGS(&pipeline_state)));

  auto lut_view_root_signature =
      d3d12::CreateRootSignature(d3d_device, lut_view_shader);
  D3D12_COMPUTE_PIPELINE_STATE_DESC compute_desc = {};
  compute_desc.pRootSignature = lut_view_root_signature.Get();
  compute_desc.CS = lut_view_shader.GetBytecode();
  d3d12::ComPtr<ID3D12PipelineState> lut_view_pipeline_state;
  D3D12_CHECK(d3d_device->CreateComputePipelineState(
      &compute_desc, IID_PPV_ARGS(&lut_view_pipeline_state)));

  root_signature_ = std::move(root_signature);
  pipeline_state_ = std::move(pipeline_state);
  lut_view_root_signature_ = std::move(lut_view_root_signature);
  lut_view_pipeline_state_ = std::move(lut_view_pipeline_state);
}

/*
<p>When shader source files change, we recompile the corresponding shaders. If
an atmosphere shader changed, we must also redo the precomputations (which
recompiles the precomputation shaders). In case of errors, we keep the previous
shaders, and display the error message:
*/

void Demo::ReloadShaders() {
  device_->WaitIdle();
  try {
    if (model_) {
      model_->Init(settings_.num_scattering_orders);
      BuildPipelines();
    } else {
      InitModel();
    }
    error_message_.clear();
  } catch (const std::exception& error) {
    error_message_ = error.what();
  }
}

void Demo::Update() {
  if (!shader_watcher_.GetChangedFiles().empty()) {
    ReloadShaders();
  }
  if (model_dirty_) {
    model_dirty_ = false;
    settings_modified_ = false;
    device_->WaitIdle();
    InitModel();
  }
}

/*
<p>The scene rendering method sets the constant buffer values related to the
camera position and to the Sun direction (computed as in the OpenGL demo), as
well as the descriptor heap indices of the precomputed textures, and then draws
a full screen triangle. The caller must have set the shader visible descriptor
heap on the command list:
*/

void Demo::Render(ID3D12GraphicsCommandList* command_list,
                  uint32_t frame_index,
                  D3D12_CPU_DESCRIPTOR_HANDLE render_target, uint32_t width,
                  uint32_t height) {
  if (show_lut_viewer_ && model_ && lut_view_pipeline_state_) {
    RenderLutView(command_list);
  }
  const float kClearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  command_list->OMSetRenderTargets(1, &render_target, FALSE, nullptr);
  command_list->ClearRenderTargetView(render_target, kClearColor, 0, nullptr);
  if (!model_ || !pipeline_state_ || width == 0 || height == 0) {
    return;
  }

  DemoConstants constants = {};

  // Transform matrix from clip space to camera space (i.e. the inverse of a
  // projection matrix).
  const float kFovY = static_cast<float>(50.0 / 180.0 * kPi);
  const float kTanFovY = tan(kFovY / 2.0f);
  float aspect_ratio = static_cast<float>(width) / height;
  const float view_from_clip[16] = {
    kTanFovY * aspect_ratio, 0.0, 0.0, 0.0,
    0.0, kTanFovY, 0.0, 0.0,
    0.0, 0.0, 0.0, -1.0,
    0.0, 0.0, 1.0, 1.0
  };
  memcpy(constants.view_from_clip, view_from_clip, sizeof(view_from_clip));

  // Unit vectors of the camera frame, expressed in world space.
  float cos_z = static_cast<float>(cos(view_zenith_angle_radians_));
  float sin_z = static_cast<float>(sin(view_zenith_angle_radians_));
  float cos_a = static_cast<float>(cos(view_azimuth_angle_radians_));
  float sin_a = static_cast<float>(sin(view_azimuth_angle_radians_));
  float ux[3] = { -sin_a, cos_a, 0.0 };
  float uy[3] = { -cos_z * cos_a, -cos_z * sin_a, sin_z };
  float uz[3] = { sin_z * cos_a, sin_z * sin_a, cos_z };
  float l = static_cast<float>(view_distance_meters_ / kLengthUnitInMeters);

  // Transform matrix from camera frame to world space (i.e. the inverse of a
  // view matrix).
  const float model_from_view[16] = {
    ux[0], uy[0], uz[0], uz[0] * l,
    ux[1], uy[1], uz[1], uz[1] * l,
    ux[2], uy[2], uz[2], uz[2] * l,
    0.0, 0.0, 0.0, 1.0
  };
  memcpy(constants.model_from_view, model_from_view, sizeof(model_from_view));

  constants.camera[0] = model_from_view[3];
  constants.camera[1] = model_from_view[7];
  constants.camera[2] = model_from_view[11];
  constants.exposure = static_cast<float>(
      use_luminance_ != NONE ? exposure_ * 1e-5 : exposure_);
  memcpy(constants.white_point, white_point_, sizeof(white_point_));
  constants.earth_center[2] =
      static_cast<float>(-settings_.bottom_radius / kLengthUnitInMeters);
  constants.sun_direction[0] = static_cast<float>(
      cos(sun_azimuth_angle_radians_) * sin(sun_zenith_angle_radians_));
  constants.sun_direction[1] = static_cast<float>(
      sin(sun_azimuth_angle_radians_) * sin(sun_zenith_angle_radians_));
  constants.sun_direction[2] =
      static_cast<float>(cos(sun_zenith_angle_radians_));
  constants.sun_size[0] =
      static_cast<float>(tan(settings_.sun_angular_radius));
  constants.sun_size[1] =
      static_cast<float>(cos(settings_.sun_angular_radius));
  constants.sky_spectral_radiance_to_luminance =
      model_->sky_spectral_radiance_to_luminance();
  constants.sun_spectral_radiance_to_luminance =
      model_->sun_spectral_radiance_to_luminance();
  constants.atmosphere = model_->atmosphere_parameters();
  memcpy(constants_data_ + frame_index * kConstantsSize, &constants,
         sizeof(constants));

  // When the scattering textures are combined, the scattering texture is
  // passed as the (unused) single Mie scattering texture.
  const d3d12::Texture* single_mie_scattering_texture =
      model_->optional_single_mie_scattering_texture();
  DemoResources resources = {
    model_->transmittance_texture().srv(),
    model_->scattering_texture().srv(),
    single_mie_scattering_texture ? single_mie_scattering_texture->srv()
                                  : model_->scattering_texture().srv(),
    model_->irradiance_texture().srv()
  };

  D3D12_VIEWPORT viewport = {0.0f, 0.0f, static_cast<float>(width),
                             static_cast<float>(height), 0.0f, 1.0f};
  D3D12_RECT scissor = {0, 0, static_cast<LONG>(width),
                        static_cast<LONG>(height)};
  command_list->SetGraphicsRootSignature(root_signature_.Get());
  command_list->SetPipelineState(pipeline_state_.Get());
  command_list->SetGraphicsRootConstantBufferView(
      0, constants_buffer_->GetGPUVirtualAddress() +
             frame_index * kConstantsSize);
  command_list->SetGraphicsRoot32BitConstants(1, 4, &resources, 0);
  command_list->RSSetViewports(1, &viewport);
  command_list->RSSetScissorRects(1, &scissor);
  command_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  command_list->DrawInstanced(3, 1, 0, 0);
}

/*
<p>The LUT viewer copies the selected precomputed texture (or 3D texture slice)
into an 8 bits texture which can then be displayed with Dear ImGui:
*/

void Demo::RenderLutView(ID3D12GraphicsCommandList* command_list) {
  const d3d12::Texture* textures[] = {
    &model_->transmittance_texture(),
    &model_->irradiance_texture(),
    &model_->scattering_texture(),
    model_->optional_single_mie_scattering_texture(),
  };
  const d3d12::Texture* source = textures[lut_index_];
  if (source == nullptr) {
    return;
  }
  LutViewParameters parameters = {};
  parameters.source_texture = source->srv();
  parameters.is_3d = source->is_3d() ? 1 : 0;
  parameters.slice = static_cast<uint32_t>(lut_slice_);
  parameters.show_alpha = lut_show_alpha_ ? 1 : 0;
  parameters.scale = lut_scale_;
  parameters.output_texture = lut_view_texture_.uav();
  parameters.width = static_cast<uint32_t>(source->desc().Width);
  parameters.height = source->desc().Height;

  d3d12::Transition(command_list, {
      {&lut_view_texture_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS}});
  command_list->SetComputeRootSignature(lut_view_root_signature_.Get());
  command_list->SetPipelineState(lut_view_pipeline_state_.Get());
  command_list->SetComputeRoot32BitConstants(0, 8, &parameters, 0);
  command_list->Dispatch((parameters.width + 7) / 8,
                         (parameters.height + 7) / 8, 1);
  d3d12::Transition(command_list, {
      {&lut_view_texture_, d3d12::Texture::kShaderResourceState}});
}

/*
<p>The other event handling methods are also straightforward, and do not
interact with the atmosphere model (except to re-initialize it when an option
changes). The keyboard shortcuts are the same as in the OpenGL demo:
*/

void Demo::HandleKey(char key) {
  if (key == 'h') {
    show_ui_ = !show_ui_;
  } else if (key == 'v') {
    show_lut_viewer_ = !show_lut_viewer_;
  } else if (key == 's') {
    settings_.use_constant_solar_spectrum =
        !settings_.use_constant_solar_spectrum;
  } else if (key == 'o') {
    settings_.use_ozone = !settings_.use_ozone;
  } else if (key == 't') {
    settings_.use_combined_textures = !settings_.use_combined_textures;
  } else if (key == 'p') {
    settings_.use_half_precision = !settings_.use_half_precision;
    settings_.max_sun_zenith_angle_degrees =
        settings_.use_half_precision ? 102.0 : 120.0;
  } else if (key == 'c') {
    settings_.use_rgb_coefficients = !settings_.use_rgb_coefficients;
  } else if (key == 'l') {
    // Only the radiance mode is available with RGB coefficients.
    if (model_uses_rgb_coefficients_) {
      return;
    }
    switch (use_luminance_) {
      case NONE: use_luminance_ = APPROXIMATE; break;
      case APPROXIMATE: use_luminance_ = PRECOMPUTED; break;
      case PRECOMPUTED: use_luminance_ = NONE; break;
    }
  } else if (key == 'w') {
    do_white_balance_ = !do_white_balance_;
    UpdateWhitePoint();
  } else if (key == '+') {
    exposure_ *= 1.1;
  } else if (key == '-') {
    exposure_ /= 1.1;
  } else if (key == '1') {
    SetView(9000.0, 1.47, 0.0, 1.3, 3.0, 10.0);
  } else if (key == '2') {
    SetView(9000.0, 1.47, 0.0, 1.564, -3.0, 10.0);
  } else if (key == '3') {
    SetView(7000.0, 1.57, 0.0, 1.54, -2.96, 10.0);
  } else if (key == '4') {
    SetView(7000.0, 1.57, 0.0, 1.328, -3.044, 10.0);
  } else if (key == '5') {
    SetView(9000.0, 1.39, 0.0, 1.2, 0.7, 10.0);
  } else if (key == '6') {
    SetView(9000.0, 1.5, 0.0, 1.628, 1.05, 200.0);
  } else if (key == '7') {
    SetView(7000.0, 1.43, 0.0, 1.57, 1.34, 40.0);
  } else if (key == '8') {
    SetView(2.7e6, 0.81, 0.0, 1.57, 2.0, 10.0);
  } else if (key == '9') {
    SetView(1.2e7, 0.0, 0.0, 0.93, -2.0, 10.0);
  }
  if (key == 's' || key == 'o' || key == 't' || key == 'p' || key == 'l' ||
      key == 'c') {
    model_dirty_ = true;
  }
}

void Demo::HandleMouseDrag(int delta_x, int delta_y, bool move_sun) {
  constexpr double kScale = 500.0;
  if (move_sun) {
    sun_zenith_angle_radians_ += delta_y / kScale;
    sun_zenith_angle_radians_ =
        std::max(0.0, std::min(kPi, sun_zenith_angle_radians_));
    sun_azimuth_angle_radians_ -= delta_x / kScale;
  } else {
    view_zenith_angle_radians_ -= delta_y / kScale;
    view_zenith_angle_radians_ =
        std::max(0.0, std::min(kPi / 2.0, view_zenith_angle_radians_));
    view_azimuth_angle_radians_ -= delta_x / kScale;
  }
}

void Demo::HandleMouseWheel(int direction) {
  if (direction < 0) {
    view_distance_meters_ *= 1.05;
  } else {
    view_distance_meters_ /= 1.05;
  }
}

void Demo::SetView(double view_distance_meters,
    double view_zenith_angle_radians, double view_azimuth_angle_radians,
    double sun_zenith_angle_radians, double sun_azimuth_angle_radians,
    double exposure) {
  view_distance_meters_ = view_distance_meters;
  view_zenith_angle_radians_ = view_zenith_angle_radians;
  view_azimuth_angle_radians_ = view_azimuth_angle_radians;
  sun_zenith_angle_radians_ = sun_zenith_angle_radians;
  sun_azimuth_angle_radians_ = sun_azimuth_angle_radians;
  exposure_ = exposure;
}

}  // namespace demo
}  // namespace atmosphere
