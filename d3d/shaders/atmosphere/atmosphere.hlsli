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

/*<h2>shaders/atmosphere/atmosphere.hlsli</h2>

<p>This file provides the public API of our atmosphere model, i.e. the functions
that client shaders call to render the sky, the aerial perspective, and the
ground lighting. It is the HLSL equivalent of the <code>kAtmosphereShader</code>
GLSL shader of <code>atmosphere/model.cc</code>.

<p>In the GLSL version, the atmosphere parameters and the precomputed textures
are global constants and uniforms of this shader, which is linked with the
client shaders. In HLSL, in order to stay independent of any resource binding
scheme (and thus usable as is in any project), the API functions take all of
them as explicit arguments instead:
<ul>
<li><code>atmosphere</code>: the atmosphere parameters, as provided by the C++
<code>atmosphere::Model</code> class (for the wavelengths
<code>kLambdaR</code>, <code>kLambdaG</code>, <code>kLambdaB</code>),</li>
<li><code>transmittance_texture</code>, <code>scattering_texture</code>,
<code>single_mie_scattering_texture</code> and <code>irradiance_texture</code>:
the precomputed textures (when the <code>COMBINED_SCATTERING_TEXTURES</code>
macro is defined, <code>single_mie_scattering_texture</code> is not used and
any texture can be passed instead, for instance
<code>scattering_texture</code>),</li>
<li><code>lut_sampler</code>: a linear filtering, clamp-to-edge sampler, used
for all the precomputed texture lookups,</li>
<li><code>sky_spectral_radiance_to_luminance</code> and
<code>sun_spectral_radiance_to_luminance</code>: the conversion factors from
spectral radiance to luminance, for the sky and for the Sun (only needed for
the functions returning luminance values), also provided by the C++
<code>atmosphere::Model</code> class.</li>
</ul>

<p>The other arguments, as well as the meaning of the return values, are
explained in the C++ <a href="../../src/atmosphere/model.h.html">model.h</a>
header. In summary, <code>camera</code> and <code>p</code> must be expressed in
a reference frame where the planet center is at the origin, and measured in the
length unit used for the atmosphere parameters. The functions returning
radiance values are only provided when the <code>RADIANCE_API_ENABLED</code>
macro is defined (i.e. when the precomputed textures contain radiance values,
and not luminance values).
*/

#ifndef ATMOSPHERE_ATMOSPHERE_HLSLI_
#define ATMOSPHERE_ATMOSPHERE_HLSLI_

#include "functions.hlsli"

/*
<p>With explicit arguments, the radiance API functions
<code>GetSkyRadiance</code>, <code>GetSkyRadianceToPoint</code> and
<code>GetSunAndSkyIrradiance</code> have exactly the same signatures as the
functions of the same name in
<a href="functions.hlsli.html">functions.hlsli</a>, which can therefore be used
directly (they are only meaningful when <code>RADIANCE_API_ENABLED</code> is
defined). We only need to add the following one:
*/

#ifdef RADIANCE_API_ENABLED

float3 GetSolarRadiance(AtmosphereParameters atmosphere) {
  return atmosphere.solar_irradiance /
      (PI * atmosphere.sun_angular_radius * atmosphere.sun_angular_radius);
}

#endif  // RADIANCE_API_ENABLED

/*
<p>The luminance API functions, on the other hand, convert the results of the
above functions with the radiance to luminance conversion factors:
*/

float3 GetSolarLuminance(
    AtmosphereParameters atmosphere,
    float3 sun_spectral_radiance_to_luminance) {
  return atmosphere.solar_irradiance /
      (PI * atmosphere.sun_angular_radius * atmosphere.sun_angular_radius) *
      sun_spectral_radiance_to_luminance;
}

float3 GetSkyLuminance(
    AtmosphereParameters atmosphere,
    float3 sky_spectral_radiance_to_luminance,
    Texture2D<float4> transmittance_texture,
    Texture3D<float4> scattering_texture,
    Texture3D<float4> single_mie_scattering_texture,
    SamplerState lut_sampler,
    float3 camera, float3 view_ray, float shadow_length,
    float3 sun_direction, out float3 transmittance) {
  return GetSkyRadiance(atmosphere, transmittance_texture,
      scattering_texture, single_mie_scattering_texture, lut_sampler,
      camera, view_ray, shadow_length, sun_direction, transmittance) *
      sky_spectral_radiance_to_luminance;
}

float3 GetSkyLuminanceToPoint(
    AtmosphereParameters atmosphere,
    float3 sky_spectral_radiance_to_luminance,
    Texture2D<float4> transmittance_texture,
    Texture3D<float4> scattering_texture,
    Texture3D<float4> single_mie_scattering_texture,
    SamplerState lut_sampler,
    float3 camera, float3 p, float shadow_length,
    float3 sun_direction, out float3 transmittance) {
  return GetSkyRadianceToPoint(atmosphere, transmittance_texture,
      scattering_texture, single_mie_scattering_texture, lut_sampler,
      camera, p, shadow_length, sun_direction, transmittance) *
      sky_spectral_radiance_to_luminance;
}

float3 GetSunAndSkyIlluminance(
    AtmosphereParameters atmosphere,
    float3 sky_spectral_radiance_to_luminance,
    float3 sun_spectral_radiance_to_luminance,
    Texture2D<float4> transmittance_texture,
    Texture2D<float4> irradiance_texture,
    SamplerState lut_sampler,
    float3 p, float3 normal, float3 sun_direction,
    out float3 sky_illuminance) {
  float3 sun_irradiance = GetSunAndSkyIrradiance(atmosphere,
      transmittance_texture, irradiance_texture, lut_sampler, p, normal,
      sun_direction, sky_illuminance);
  sky_illuminance *= sky_spectral_radiance_to_luminance;
  return sun_irradiance * sun_spectral_radiance_to_luminance;
}

/*
<p>With the <code>spectral_sun_transmittance</code> option of the C++
<code>atmosphere::Model</code> class (i.e. when the
<code>SPECTRAL_SUN_TRANSMITTANCE</code> macro is defined), an additional
precomputed texture, <code>sun_transmittance_texture</code>, contains the
"effective" transmittance of the solar spectrum between any point and the top
atmosphere boundary, for each linear sRGB channel (see
<a href="../../src/atmosphere/model.cc.html#sun_transmittance">model.cc</a>).
Multiplied with <code>GetSolarLuminance</code>, it gives the exact color of the
Sun light at this point, unlike the (normal) transmittance texture, which only
contains the transmittance for 3 extinction coefficients. It has the same
parameterization as the transmittance texture, so the functions of
<a href="functions.hlsli.html">functions.hlsli</a> for the transmittance to the
top atmosphere boundary can be used with it. The following function uses them
to get the transmittance of the Sun light for a viewer at <code>camera</code>
(possibly in space) looking in the given direction, which is 0 if the view ray
intersects the ground (as for the transmittance returned by
<code>GetSkyLuminance</code>), and 1 if it does not intersect the atmosphere:
*/

#ifdef SPECTRAL_SUN_TRANSMITTANCE

float3 GetSunTransmittance(
    AtmosphereParameters atmosphere,
    Texture2D<float4> sun_transmittance_texture,
    SamplerState lut_sampler,
    float3 camera, float3 direction) {
  // Move the viewer to the top atmosphere boundary if it is in space, as in
  // GetSkyRadiance.
  float r = length(camera);
  float rmu = dot(camera, direction);
  float distance_to_top_atmosphere_boundary = -rmu -
      sqrt(rmu * rmu - r * r + atmosphere.top_radius * atmosphere.top_radius);
  if (distance_to_top_atmosphere_boundary > 0.0 * m) {
    r = atmosphere.top_radius;
    rmu += distance_to_top_atmosphere_boundary;
  } else if (r > atmosphere.top_radius) {
    return (float3)1.0;
  }
  float mu = rmu / r;
  return RayIntersectsGround(atmosphere, r, mu) ? (float3)0.0 :
      GetTransmittanceToTopAtmosphereBoundary(
          atmosphere, sun_transmittance_texture, lut_sampler, r, mu);
}

/*
<p>Similarly, the transmittance texture is only used for the direct sunlight in
<code>GetSunAndSkyIrradiance</code> (via <code>GetTransmittanceToSun</code>,
which also takes into account the fraction of the Sun disc above the horizon),
so we can get the exact color of the direct sunlight by passing the sun
transmittance texture instead:
*/

float3 GetSpectralSunAndSkyIlluminance(
    AtmosphereParameters atmosphere,
    float3 sky_spectral_radiance_to_luminance,
    float3 sun_spectral_radiance_to_luminance,
    Texture2D<float4> sun_transmittance_texture,
    Texture2D<float4> irradiance_texture,
    SamplerState lut_sampler,
    float3 p, float3 normal, float3 sun_direction,
    out float3 sky_illuminance) {
  return GetSunAndSkyIlluminance(atmosphere,
      sky_spectral_radiance_to_luminance, sun_spectral_radiance_to_luminance,
      sun_transmittance_texture, irradiance_texture, lut_sampler, p, normal,
      sun_direction, sky_illuminance);
}

#endif  // SPECTRAL_SUN_TRANSMITTANCE

#endif  // ATMOSPHERE_ATMOSPHERE_HLSLI_
