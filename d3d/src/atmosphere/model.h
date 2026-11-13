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

/*<h2>src/atmosphere/model.h</h2>

<p>This file defines the API to use our atmosphere model in Direct3D 12
applications. It is a port of the OpenGL <code>atmosphere/model.h</code> API.
To use it:
<ul>
<li>create a <code>Model</code> instance with the desired atmosphere
parameters,</li>
<li>call <code>Init</code> to precompute the atmosphere textures,</li>
<li>compile your shaders that need access to the atmosphere shading functions
with the preprocessor macros returned by <code>shader_defines()</code>, and
include <a href="../../shaders/atmosphere/atmosphere.hlsli.html"
>shaders/atmosphere/atmosphere.hlsli</a> in them,</li>
<li>provide the atmosphere parameters (<code>atmosphere_parameters()</code>),
the radiance to luminance conversion factors, and the precomputed textures to
these shaders (with constant buffers and shader resource views - this class
creates a shader resource view of each precomputed texture in the descriptor
heap given to its constructor),</li>
<li>delete your <code>Model</code> when you no longer need its precomputed
textures (and after the GPU has finished using them).</li>
</ul>

<p>The <code>atmosphere.hlsli</code> file provides the following functions:

<pre class="prettyprint">
// Returns the radiance of the Sun, outside the atmosphere.
float3 GetSolarRadiance(AtmosphereParameters atmosphere);

// Returns the sky radiance along the segment from 'camera' to the nearest
// atmosphere boundary in direction 'view_ray', as well as the transmittance
// along this segment.
float3 GetSkyRadiance(AtmosphereParameters atmosphere,
    Texture2D&lt;float4&gt; transmittance_texture,
    Texture3D&lt;float4&gt; scattering_texture,
    Texture3D&lt;float4&gt; single_mie_scattering_texture,
    SamplerState lut_sampler,
    float3 camera, float3 view_ray, float shadow_length,
    float3 sun_direction, out float3 transmittance);

// Returns the sky radiance along the segment from 'camera' to 'p', as well as
// the transmittance along this segment.
float3 GetSkyRadianceToPoint(AtmosphereParameters atmosphere,
    Texture2D&lt;float4&gt; transmittance_texture,
    Texture3D&lt;float4&gt; scattering_texture,
    Texture3D&lt;float4&gt; single_mie_scattering_texture,
    SamplerState lut_sampler,
    float3 camera, float3 p, float shadow_length,
    float3 sun_direction, out float3 transmittance);

// Returns the sun and sky irradiance received on a surface patch located at 'p'
// and whose normal vector is 'normal'.
float3 GetSunAndSkyIrradiance(AtmosphereParameters atmosphere,
    Texture2D&lt;float4&gt; transmittance_texture,
    Texture2D&lt;float4&gt; irradiance_texture,
    SamplerState lut_sampler,
    float3 p, float3 normal, float3 sun_direction,
    out float3 sky_irradiance);

// Returns the luminance of the Sun, outside the atmosphere.
float3 GetSolarLuminance(AtmosphereParameters atmosphere,
    float3 sun_spectral_radiance_to_luminance);

// Returns the sky luminance along the segment from 'camera' to the nearest
// atmosphere boundary in direction 'view_ray', as well as the transmittance
// along this segment.
float3 GetSkyLuminance(AtmosphereParameters atmosphere,
    float3 sky_spectral_radiance_to_luminance,
    Texture2D&lt;float4&gt; transmittance_texture,
    Texture3D&lt;float4&gt; scattering_texture,
    Texture3D&lt;float4&gt; single_mie_scattering_texture,
    SamplerState lut_sampler,
    float3 camera, float3 view_ray, float shadow_length,
    float3 sun_direction, out float3 transmittance);

// Returns the sky luminance along the segment from 'camera' to 'p', as well as
// the transmittance along this segment.
float3 GetSkyLuminanceToPoint(AtmosphereParameters atmosphere,
    float3 sky_spectral_radiance_to_luminance,
    Texture2D&lt;float4&gt; transmittance_texture,
    Texture3D&lt;float4&gt; scattering_texture,
    Texture3D&lt;float4&gt; single_mie_scattering_texture,
    SamplerState lut_sampler,
    float3 camera, float3 p, float shadow_length,
    float3 sun_direction, out float3 transmittance);

// Returns the sun and sky illuminance received on a surface patch located at
// 'p' and whose normal vector is 'normal'.
float3 GetSunAndSkyIlluminance(AtmosphereParameters atmosphere,
    float3 sky_spectral_radiance_to_luminance,
    float3 sun_spectral_radiance_to_luminance,
    Texture2D&lt;float4&gt; transmittance_texture,
    Texture2D&lt;float4&gt; irradiance_texture,
    SamplerState lut_sampler,
    float3 p, float3 normal, float3 sun_direction,
    out float3 sky_illuminance);

// Only if SPECTRAL_SUN_TRANSMITTANCE is defined (see below): returns the
// transmittance of the solar spectrum between 'camera' and the top atmosphere
// boundary in direction 'direction', as a linear sRGB color ratio (to be
// multiplied with GetSolarLuminance).
float3 GetSunTransmittance(AtmosphereParameters atmosphere,
    Texture2D&lt;float4&gt; sun_transmittance_texture,
    SamplerState lut_sampler,
    float3 camera, float3 direction);

// Only if SPECTRAL_SUN_TRANSMITTANCE is defined: same as
// GetSunAndSkyIlluminance, but with the sun illuminance computed with the
// spectral sun transmittance texture.
float3 GetSpectralSunAndSkyIlluminance(AtmosphereParameters atmosphere,
    float3 sky_spectral_radiance_to_luminance,
    float3 sun_spectral_radiance_to_luminance,
    Texture2D&lt;float4&gt; sun_transmittance_texture,
    Texture2D&lt;float4&gt; irradiance_texture,
    SamplerState lut_sampler,
    float3 p, float3 normal, float3 sun_direction,
    out float3 sky_illuminance);
</pre>

<p>where
<ul>
<li><code>atmosphere</code> is the <code>AtmosphereParameters</code> struct
returned by <code>atmosphere_parameters()</code>,</li>
<li><code>sky_spectral_radiance_to_luminance</code> and
<code>sun_spectral_radiance_to_luminance</code> are the values returned by the
methods of the same name,</li>
<li><code>lut_sampler</code> is a linear filtering, clamp-to-edge sampler,</li>
<li>when <code>combine_scattering_textures()</code> is true, there is no single
Mie scattering texture, and any texture can be passed for the
<code>single_mie_scattering_texture</code> argument (it is not used),</li>
<li><code>camera</code> and <code>p</code> must be expressed in a reference
frame where the planet center is at the origin, and measured in the unit passed
to the constructor's <code>length_unit_in_meters</code> argument.
<code>camera</code> can be in space, but <code>p</code> must be inside the
atmosphere,</li>
<li><code>view_ray</code>, <code>sun_direction</code> and <code>normal</code>
are unit direction vectors expressed in the same reference frame (with
<code>sun_direction</code> pointing <i>towards</i> the Sun),</li>
<li><code>shadow_length</code> is the length along the segment which is in
shadow, measured in the unit passed to the constructor's
<code>length_unit_in_meters</code> argument.</li>
</ul>

<p>and where
<ul>
<li>the first 4 functions return spectral radiance and irradiance values
(in $W.m^{-2}.sr^{-1}.nm^{-1}$ and $W.m^{-2}.nm^{-1}$), at the 3 wavelengths
<code>kLambdaR</code>, <code>kLambdaG</code>, <code>kLambdaB</code> (in this
order),</li>
<li>the other functions return luminance and illuminance values (in
$cd.m^{-2}$ and $lx$) in linear <a href="https://en.wikipedia.org/wiki/SRGB">
sRGB</a> space (i.e. before adjustements for gamma correction),</li>
<li>all the functions with a <code>transmittance</code> output parameter
return in it the (unitless) transmittance of the atmosphere along the specified
segment at the 3 wavelengths <code>kLambdaR</code>, <code>kLambdaG</code>,
<code>kLambdaB</code> (in this order) - or, with the
<code>spectral_sun_transmittance</code> option (see below), for extinction
coefficients averaged over the linear sRGB channels,</li>
<li><code>GetSunTransmittance</code> returns a (unitless) effective
transmittance for each linear sRGB channel, i.e. the ratio between the linear
sRGB color of the solar spectrum attenuated by the atmosphere along the
specified ray, and the one of the unattenuated solar spectrum.</li>
</ul>

<p><b>Note</b> The precomputed atmosphere textures can store either irradiance
or illuminance values (see the <code>num_precomputed_wavelengths</code>
parameter):
<ul>
  <li>when using irradiance values, the RGB channels of these textures contain
  spectral irradiance values, in $W.m^{-2}.nm^{-1}$, at the 3 wavelengths
  <code>kLambdaR</code>, <code>kLambdaG</code>, <code>kLambdaB</code> (in this
  order). The API functions returning radiance values return these precomputed
  values (times the phase functions), while the API functions returning
  luminance values use the approximation described in
  <a href="https://arxiv.org/pdf/1612.04336.pdf">A Qualitative and Quantitative
  Evaluation of 8 Clear Sky Models</a>, section 14.3, to convert 3 radiance
  values to linear sRGB luminance values.</li>
  <li>when using illuminance values, the RGB channels of these textures contain
  illuminance values, in $lx$, in linear sRGB space. These illuminance values
  are precomputed as described in
  <a href="http://www.oskee.wz.cz/stranka/uploads/SCCG10ElekKmoch.pdf">Real-time
  Spectral Scattering in Large-scale Natural Participating Media</a>, section
  4.4 (i.e. <code>num_precomputed_wavelengths</code> irradiance values are
  precomputed - or 6 or 9 values with a fitted <code>WavelengthRule</code>, see
  below - and then converted to sRGB via a numerical integration of this
  spectrum with the CIE color matching functions). The API functions returning
  luminance values return these precomputed values (times the phase functions),
  while <i>the API functions returning radiance values are not provided</i>
  (i.e. the <code>RADIANCE_API_ENABLED</code> macro is not defined).
  </li>
</ul>

<p><b>Note</b> In precomputed illuminance mode, the scattering textures are
integrated over the whole spectrum, but the transmittance texture (used for the
Sun disc, the direct sunlight on surfaces, and the aerial perspective) still
contains the transmittance at 3 wavelengths only, which gives wrong colors and
luminances for long optical paths (e.g. at sunset). This D3D12 port provides an
option, <code>spectral_sun_transmittance</code>, to reduce these errors. With
this option the transmittance texture is computed with extinction coefficients
averaged over the linear sRGB channels (instead of the values at
<code>kLambdaR</code>, <code>kLambdaG</code>, <code>kLambdaB</code>), and an
additional "spectral sun transmittance" texture is precomputed, giving the
exact linear sRGB color of the Sun light after its attenuation from the top of
the atmosphere to any point (see <a href="model.cc.html#sun_transmittance"
>model.cc</a> for details). The shaders must then be compiled with the
<code>SPECTRAL_SUN_TRANSMITTANCE</code> macro (included in
<code>shader_defines()</code>), and should use <code>GetSunTransmittance</code>
for the Sun disc and <code>GetSpectralSunAndSkyIlluminance</code> instead of
<code>GetSunAndSkyIlluminance</code> (the other functions are unchanged).

<p>The concrete API definition is the following. We start with the C++ mirrors
of the HLSL structs used in constant buffers, which must follow the HLSL
constant buffer packing rules (in particular, each struct and each array element
starts on a 16 bytes boundary, but the last array element is not padded):
*/

#ifndef D3D_ATMOSPHERE_MODEL_H_
#define D3D_ATMOSPHERE_MODEL_H_

#include <array>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "d3d12/common.h"
#include "d3d12/descriptor_heap.h"
#include "d3d12/device.h"
#include "d3d12/gpu_timer.h"
#include "d3d12/shader_compiler.h"
#include "d3d12/texture.h"

namespace atmosphere {

namespace hlsl {

using float3 = std::array<float, 3>;

struct DensityProfileLayer {
  float width;
  float exp_term;
  float exp_scale;
  float linear_term;
  float constant_term;
};

// The HLSL DensityProfile struct contains a DensityProfileLayer layers[2]
// array, whose second element starts 32 bytes after the first one.
struct DensityProfile {
  DensityProfileLayer layer0;
  float padding[3];
  DensityProfileLayer layer1;
};

struct AtmosphereParameters {
  float3 solar_irradiance;
  float sun_angular_radius;
  float bottom_radius;
  float top_radius;
  float padding[2];
  DensityProfile rayleigh_density;
  float3 rayleigh_scattering;
  DensityProfile mie_density;
  float3 mie_scattering;
  float3 mie_extinction;
  float mie_phase_function_g;
  DensityProfile absorption_density;
  float3 absorption_extinction;
  float3 ground_albedo;
  float mu_s_min;
};

static_assert(sizeof(DensityProfile) == 52);
static_assert(offsetof(AtmosphereParameters, rayleigh_density) == 32);
static_assert(offsetof(AtmosphereParameters, rayleigh_scattering) == 84);
static_assert(offsetof(AtmosphereParameters, mie_density) == 96);
static_assert(offsetof(AtmosphereParameters, mie_scattering) == 148);
static_assert(offsetof(AtmosphereParameters, mie_extinction) == 160);
static_assert(offsetof(AtmosphereParameters, absorption_density) == 176);
static_assert(offsetof(AtmosphereParameters, absorption_extinction) == 228);
static_assert(offsetof(AtmosphereParameters, ground_albedo) == 240);
static_assert(sizeof(AtmosphereParameters) == 256);

// Returns the expected offsets of the AtmosphereParameters members, in a
// constant buffer where an AtmosphereParameters struct named 'name' starts at
// 'offset' (to check the C++ layout against the shader reflection data).
std::vector<d3d12::ConstantBufferMember> GetAtmosphereParametersLayout(
    const std::string& name, size_t offset);

}  // namespace hlsl

/*
<p>The <code>DensityProfileLayer</code> class, used to specify the atmosphere
density profiles, is unchanged from the OpenGL version:
*/

// An atmosphere layer of width 'width' (in m), and whose density is defined as
//   'exp_term' * exp('exp_scale' * h) + 'linear_term' * h + 'constant_term',
// clamped to [0,1], and where h is the altitude (in m). 'exp_term' and
// 'constant_term' are unitless, while 'exp_scale' and 'linear_term' are in
// m^-1.
class DensityProfileLayer {
 public:
  DensityProfileLayer() : DensityProfileLayer(0.0, 0.0, 0.0, 0.0, 0.0) {}
  DensityProfileLayer(double width, double exp_term, double exp_scale,
                      double linear_term, double constant_term)
      : width(width), exp_term(exp_term), exp_scale(exp_scale),
        linear_term(linear_term), constant_term(constant_term) {
  }
  double width;
  double exp_term;
  double exp_scale;
  double linear_term;
  double constant_term;
};

/*
<p>In precomputed illuminance mode (see above), the precomputed sRGB values are
a numerical integral, over the wavelength, of the spectral results at a few
wavelengths. This D3D12 port adds an option to choose the integration rule (the
OpenGL version only provides the first one):
*/

enum class WavelengthRule {
  // num_precomputed_wavelengths (rounded up to a multiple of 3) wavelengths,
  // evenly spaced between 360 and 830nm, each weighted by the sRGB color
  // matching functions at this wavelength times the wavelength spacing (as in
  // the OpenGL version).
  kMidpoint,
  // 6 (resp. 9) wavelengths at fixed positions, optimized offline, with
  // weights fitted at construction time to the model's own input spectra
  // (see model.cc). With 2 (resp. 3) precomputations of 3 wavelengths, this
  // gives colors close to converged ones for Earth-like atmospheres, much
  // closer than the default 15 wavelengths midpoint rule (5 precomputations).
  // The positions were optimized for the default Earth atmosphere: with very
  // different spectra (e.g. a much larger amount of ozone), a midpoint rule
  // with many wavelengths (30-60) can be more accurate.
  kFitted6,
  kFitted9
};

/*
<p>The <code>Model</code> constructor takes the same atmosphere parameters as
in the OpenGL version, plus the Direct3D objects needed to create the
precomputed textures, and to compile the precomputation shaders:
*/

class Model {
 public:
  Model(
    // The device used to create the precomputed textures, and to run the
    // precomputations.
    d3d12::Device* device,
    // The shader visible CBV/SRV/UAV descriptor heap where the shader resource
    // views of the precomputed textures must be created.
    d3d12::DescriptorHeap* descriptor_heap,
    // The compiler used to compile the precomputation shaders, whose shader
    // directory must contain the "atmosphere/precompute.hlsl" file.
    const d3d12::ShaderCompiler* shader_compiler,
    // The wavelength values, in nanometers, and sorted in increasing order, for
    // which the solar_irradiance, rayleigh_scattering, mie_scattering,
    // mie_extinction and ground_albedo samples are provided. If your shaders
    // use luminance values (as opposed to radiance values, see above), use a
    // large number of wavelengths (e.g. between 15 and 50) to get accurate
    // results (this number of wavelengths has absolutely no impact on the
    // shader performance).
    const std::vector<double>& wavelengths,
    // The solar irradiance at the top of the atmosphere, in W/m^2/nm. This
    // vector must have the same size as the wavelengths parameter.
    const std::vector<double>& solar_irradiance,
    // The sun's angular radius, in radians. Warning: the implementation uses
    // approximations that are valid only if this value is smaller than 0.1.
    double sun_angular_radius,
    // The distance between the planet center and the bottom of the atmosphere,
    // in m.
    double bottom_radius,
    // The distance between the planet center and the top of the atmosphere,
    // in m.
    double top_radius,
    // The density profile of air molecules, i.e. a function from altitude to
    // dimensionless values between 0 (null density) and 1 (maximum density).
    // Layers must be sorted from bottom to top. The width of the last layer is
    // ignored, i.e. it always extend to the top atmosphere boundary. At most 2
    // layers can be specified.
    const std::vector<DensityProfileLayer>& rayleigh_density,
    // The scattering coefficient of air molecules at the altitude where their
    // density is maximum (usually the bottom of the atmosphere), as a function
    // of wavelength, in m^-1. The scattering coefficient at altitude h is equal
    // to 'rayleigh_scattering' times 'rayleigh_density' at this altitude. This
    // vector must have the same size as the wavelengths parameter.
    const std::vector<double>& rayleigh_scattering,
    // The density profile of aerosols, i.e. a function from altitude to
    // dimensionless values between 0 (null density) and 1 (maximum density).
    // Layers must be sorted from bottom to top. The width of the last layer is
    // ignored, i.e. it always extend to the top atmosphere boundary. At most 2
    // layers can be specified.
    const std::vector<DensityProfileLayer>& mie_density,
    // The scattering coefficient of aerosols at the altitude where their
    // density is maximum (usually the bottom of the atmosphere), as a function
    // of wavelength, in m^-1. The scattering coefficient at altitude h is equal
    // to 'mie_scattering' times 'mie_density' at this altitude. This vector
    // must have the same size as the wavelengths parameter.
    const std::vector<double>& mie_scattering,
    // The extinction coefficient of aerosols at the altitude where their
    // density is maximum (usually the bottom of the atmosphere), as a function
    // of wavelength, in m^-1. The extinction coefficient at altitude h is equal
    // to 'mie_extinction' times 'mie_density' at this altitude. This vector
    // must have the same size as the wavelengths parameter.
    const std::vector<double>& mie_extinction,
    // The asymetry parameter for the Cornette-Shanks phase function for the
    // aerosols.
    double mie_phase_function_g,
    // The density profile of air molecules that absorb light (e.g. ozone), i.e.
    // a function from altitude to dimensionless values between 0 (null density)
    // and 1 (maximum density). Layers must be sorted from bottom to top. The
    // width of the last layer is ignored, i.e. it always extend to the top
    // atmosphere boundary. At most 2 layers can be specified.
    const std::vector<DensityProfileLayer>& absorption_density,
    // The extinction coefficient of molecules that absorb light (e.g. ozone) at
    // the altitude where their density is maximum, as a function of wavelength,
    // in m^-1. The extinction coefficient at altitude h is equal to
    // 'absorption_extinction' times 'absorption_density' at this altitude. This
    // vector must have the same size as the wavelengths parameter.
    const std::vector<double>& absorption_extinction,
    // The average albedo of the ground, as a function of wavelength. This
    // vector must have the same size as the wavelengths parameter.
    const std::vector<double>& ground_albedo,
    // The maximum Sun zenith angle for which atmospheric scattering must be
    // precomputed, in radians (for maximum precision, use the smallest Sun
    // zenith angle yielding negligible sky light radiance values. For instance,
    // for the Earth case, 102 degrees is a good choice for most cases (120
    // degrees is necessary for very high exposure values).
    double max_sun_zenith_angle,
    // The length unit used in your shaders and meshes. This is the length unit
    // which must be used when calling the atmosphere model shader functions.
    double length_unit_in_meters,
    // The number of wavelengths for which atmospheric scattering must be
    // precomputed (the temporary GPU memory used during precomputations, and
    // the GPU memory used by the precomputed results, is independent of this
    // number, but the <i>precomputation time is directly proportional to this
    // number</i>):
    // - if this number is less than or equal to 3, scattering is precomputed
    // for 3 wavelengths, and stored as irradiance values. Then both the
    // radiance-based and the luminance-based API functions are provided (see
    // the above note).
    // - otherwise, scattering is precomputed for this number of wavelengths
    // (rounded up to a multiple of 3), integrated with the CIE color matching
    // functions, and stored as illuminance values. Then only the
    // luminance-based API functions are provided (see the above note). The
    // wavelengths and the integration weights depend on wavelength_rule (with
    // a fitted rule, this number is only used to select this mode).
    unsigned int num_precomputed_wavelengths,
    // Whether to pack the (red component of the) single Mie scattering with the
    // Rayleigh and multiple scattering in a single texture, or to store the
    // (3 components of the) single Mie scattering in a separate texture.
    bool combine_scattering_textures,
    // Whether to use half precision floats (16 bits) or single precision floats
    // (32 bits) for the precomputed textures. Half precision is sufficient for
    // most cases, except for very high exposure values.
    bool half_precision,
    // Whether to accumulate the precomputation results in temporary single
    // precision textures, converted once to the final half precision textures
    // at the end of Init, instead of directly in these final textures (this
    // option has no effect if half_precision is false). In precomputed
    // illuminance mode, the scattering textures are the sum of
    // num_scattering_orders contributions for each group of 3 wavelengths, and
    // each partial sum is rounded to half precision if accumulated directly in
    // the final textures. With many wavelengths, this gives noticeable color
    // errors, that this option avoids (at the cost of more temporary GPU memory
    // during Init). It is also applied in precomputed irradiance mode, where it
    // only avoids the (smaller) rounding errors of the scattering orders sum.
    bool accumulate_in_float32 = false,
    // The wavelengths and weights used to integrate the spectral results into
    // illuminance values, in precomputed illuminance mode (ignored otherwise).
    // For Earth-like atmospheres, the fitted rules give more accurate colors
    // than the midpoint rule, with fewer wavelengths, i.e. a smaller
    // precomputation time (best used with accumulate_in_float32, since their
    // weights are larger and of both signs). If their weights can't be fitted
    // (e.g. with a null solar spectrum), the midpoint rule is used instead.
    WavelengthRule wavelength_rule = WavelengthRule::kMidpoint,
    // Whether to compute the transmittance texture with extinction
    // coefficients averaged over the linear sRGB channels, and to precompute
    // the additional spectral sun transmittance texture (see above). Only
    // used in precomputed illuminance mode (ignored otherwise).
    bool spectral_sun_transmittance = false);

  ~Model();

  Model(const Model&) = delete;
  Model& operator=(const Model&) = delete;

  // Precomputes the atmosphere textures. Blocks until the precomputations are
  // done. Can be called again (e.g. after a change in the precomputation
  // shaders), but the caller must then ensure that the GPU is not using the
  // precomputed textures. Throws a d3d12::ShaderCompilationError if the
  // precomputation shaders can't be compiled (in which case the precomputed
  // textures are left unchanged).
  void Init(unsigned int num_scattering_orders = 4);

  // The precomputed textures, in the D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE
  // state after Init. The single Mie scattering texture is null if the
  // scattering textures are combined.
  const d3d12::Texture& transmittance_texture() const {
    return transmittance_texture_;
  }
  const d3d12::Texture& scattering_texture() const {
    return scattering_texture_;
  }
  const d3d12::Texture* optional_single_mie_scattering_texture() const {
    return combine_scattering_textures_
        ? nullptr : &optional_single_mie_scattering_texture_;
  }
  const d3d12::Texture& irradiance_texture() const {
    return irradiance_texture_;
  }
  // The spectral sun transmittance texture, or null if the
  // spectral_sun_transmittance option is not used.
  const d3d12::Texture* optional_sun_transmittance_texture() const {
    return spectral_sun_transmittance_ ? &sun_transmittance_texture_ : nullptr;
  }

  // The atmosphere parameters for the kLambdaR, kLambdaG, kLambdaB
  // wavelengths, to be provided to the HLSL API functions.
  const hlsl::AtmosphereParameters& atmosphere_parameters() const {
    return atmosphere_parameters_;
  }
  const hlsl::float3& sky_spectral_radiance_to_luminance() const {
    return sky_spectral_radiance_to_luminance_;
  }
  const hlsl::float3& sun_spectral_radiance_to_luminance() const {
    return sun_spectral_radiance_to_luminance_;
  }
  bool combine_scattering_textures() const {
    return combine_scattering_textures_;
  }
  bool radiance_api_enabled() const {
    return num_precomputed_wavelengths_ <= 3;
  }
  // Whether the spectral_sun_transmittance option is used (i.e. requested, in
  // precomputed illuminance mode).
  bool spectral_sun_transmittance() const {
    return spectral_sun_transmittance_;
  }

  // The preprocessor macros that shaders including atmosphere.hlsli must be
  // compiled with (texture sizes, COMBINED_SCATTERING_TEXTURES,
  // RADIANCE_API_ENABLED and SPECTRAL_SUN_TRANSMITTANCE).
  std::vector<d3d12::ShaderDefine> shader_defines() const;

  // The GPU time, in milliseconds, spent in each precomputation pass (summed
  // over all the scattering orders and wavelengths) during the last Init.
  const std::vector<std::pair<std::string, double>>& precompute_timings()
      const {
    return precompute_timings_;
  }

  // Utility method to convert a function of the wavelength to linear sRGB.
  // 'wavelengths' and 'spectrum' must have the same size. The integral of
  // 'spectrum' times each CIE_2_DEG_COLOR_MATCHING_FUNCTIONS (and times
  // MAX_LUMINOUS_EFFICACY) is computed to get XYZ values, which are then
  // converted to linear sRGB with the XYZ_TO_SRGB matrix.
  static void ConvertSpectrumToLinearSrgb(
      const std::vector<double>& wavelengths,
      const std::vector<double>& spectrum,
      double* r, double* g, double* b);

  static constexpr double kLambdaR = 680.0;
  static constexpr double kLambdaG = 550.0;
  static constexpr double kLambdaB = 440.0;

 private:
  typedef std::array<double, 3> vec3;
  typedef std::array<float, 9> mat3;
  struct Programs;
  struct TemporaryTextures;
  // The 3 wavelengths of a precomputation pass, and the matrix converting the
  // spectral results at these wavelengths to (partial) luminance values.
  struct WavelengthPass {
    vec3 lambdas;
    mat3 luminance_from_radiance;
  };

  void Precompute(
      ID3D12GraphicsCommandList* command_list,
      d3d12::GpuTimer* timer,
      const Programs& programs,
      TemporaryTextures& textures,
      D3D12_GPU_VIRTUAL_ADDRESS constants,
      bool blend,
      unsigned int num_scattering_orders);

  d3d12::Device* device_;
  d3d12::DescriptorHeap* descriptor_heap_;
  const d3d12::ShaderCompiler* shader_compiler_;
  unsigned int num_precomputed_wavelengths_;
  bool combine_scattering_textures_;
  bool half_precision_;
  bool accumulate_in_float32_;
  // Whether the spectral_sun_transmittance option is requested and used (i.e.
  // in precomputed illuminance mode).
  bool spectral_sun_transmittance_;
  // The precomputation passes in precomputed illuminance mode (computed in
  // the constructor; empty in precomputed irradiance mode).
  std::vector<WavelengthPass> wavelength_passes_;
  std::function<hlsl::AtmosphereParameters(const vec3&)>
      atmosphere_parameters_factory_;
  hlsl::AtmosphereParameters atmosphere_parameters_;
  // With the spectral_sun_transmittance option, the atmosphere parameters used
  // to compute the final transmittance texture (atmosphere_parameters_ with
  // band-averaged extinction coefficients), and the spectral tables used to
  // compute the sun transmittance texture (see model.cc).
  hlsl::AtmosphereParameters transmittance_parameters_;
  std::vector<std::array<float, 4>> sun_transmittance_spectrum_;
  hlsl::float3 sky_spectral_radiance_to_luminance_;
  hlsl::float3 sun_spectral_radiance_to_luminance_;
  d3d12::Texture transmittance_texture_;
  d3d12::Texture scattering_texture_;
  d3d12::Texture optional_single_mie_scattering_texture_;
  d3d12::Texture irradiance_texture_;
  d3d12::Texture sun_transmittance_texture_;
  std::vector<std::pair<std::string, double>> precompute_timings_;
};

}  // namespace atmosphere

#endif  // D3D_ATMOSPHERE_MODEL_H_
