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

/*<h2>shaders/demo/demo.hlsl</h2>

<p>This HLSL file contains the shaders used to render our demo scene, which
consists of a sphere S on a purely spherical planet P. It is a port of the GLSL
<code>atmosphere/demo/demo.glsl</code> fragment shader (and of the vertex shader
defined in <code>atmosphere/demo/demo.cc</code>). The scene is rendered by "ray
tracing", i.e. the vertex shader outputs the view ray direction, and the pixel
shader computes the intersection of this ray with the spheres S and P to produce
the final pixels. The pixel shader also computes the intersection of the light
rays with the sphere S, to compute shadows, as well as the intersections of the
view ray with the shadow volume of S, in order to compute light shafts.

<p>This file is compiled by the C++ demo with the preprocessor macros of the
atmosphere model (see <code>Model::shader_defines()</code>), plus
<code>USE_LUMINANCE</code> to render luminance values instead of radiance
values, and <code>LENGTH_UNIT_IN_METERS</code>. With the
<code>SPECTRAL_SUN_TRANSMITTANCE</code> macro (only defined by the model in
precomputed luminance mode), the Sun disc and the direct sunlight use the
spectral sun transmittance texture (see
<a href="../atmosphere/atmosphere.hlsli.html">atmosphere.hlsli</a>).

<h3>Resources</h3>

<p>As for the precomputation shaders, the root signature is declared in HLSL,
and the textures are accessed with Shader Model 6.6 dynamic resources, via
descriptor heap indices provided in root constants:
*/

#define DEMO_ROOT_SIGNATURE \
    "RootFlags(CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED)," \
    "CBV(b0)," \
    "RootConstants(num32BitConstants=7, b1)," \
    "StaticSampler(s0," \
    "    filter=FILTER_MIN_MAG_MIP_LINEAR," \
    "    addressU=TEXTURE_ADDRESS_CLAMP," \
    "    addressV=TEXTURE_ADDRESS_CLAMP," \
    "    addressW=TEXTURE_ADDRESS_CLAMP)"

#include "../atmosphere/atmosphere.hlsli"

#ifndef LENGTH_UNIT_IN_METERS
#define LENGTH_UNIT_IN_METERS 1000.0
#endif

/*
<p>The constant buffer contains the equivalent of the uniforms of the GLSL
shaders, plus the atmosphere parameters and the radiance to luminance
conversion factors (which are constants of the atmosphere shader in GLSL). We
use a "classic" <code>cbuffer</code>, whose members are global variables, so
that the code below can use them exactly like the GLSL uniforms (the padding
members make the layout explicit, for the C++ code filling this buffer):
*/

cbuffer DemoConstants : register(b0) {
  row_major float4x4 model_from_view;
  row_major float4x4 view_from_clip;
  float3 camera;
  float exposure;
  float3 white_point;
  float padding0;
  float3 earth_center;
  float padding1;
  float3 sun_direction;
  float padding2;
  float2 sun_size;
  float2 padding3;
  float3 sky_spectral_radiance_to_luminance;
  float padding4;
  float3 sun_spectral_radiance_to_luminance;
  float padding5;
  AtmosphereParameters atmosphere;
};

struct DemoResources {
  uint transmittance_texture;
  uint scattering_texture;
  // Equal to scattering_texture if COMBINED_SCATTERING_TEXTURES is defined.
  uint single_mie_scattering_texture;
  uint irradiance_texture;
  // Equal to transmittance_texture if SPECTRAL_SUN_TRANSMITTANCE is not
  // defined.
  uint sun_transmittance_texture;
  // The film emulation LUT of the optional tone mapping (see below), or any 3D
  // texture if it is not available.
  uint film_lut_texture;
  // 0 for the AgX tone mapping, 1 for the film emulation LUT.
  uint tone_mapping;
};

ConstantBuffer<DemoResources> resources : register(b1);
SamplerState lut_sampler : register(s0);

/*
<h3>Vertex shader</h3>

<p>The vertex shader draws a full screen triangle (with vertices generated from
the vertex index, so that no vertex buffer is needed), and computes the view ray
of each vertex, in world space, with the inverse of the projection matrix
(<code>view_from_clip</code>) and the inverse of the view matrix
(<code>model_from_view</code>):
*/

struct VertexOutput {
  float4 position : SV_Position;
  float3 view_ray : VIEW_RAY;
};

[RootSignature(DEMO_ROOT_SIGNATURE)]
VertexOutput DemoVS(uint vertex_id : SV_VertexID) {
  float4 vertex = float4(
      vertex_id == 1 ? 3.0 : -1.0, vertex_id == 2 ? 3.0 : -1.0, 0.0, 1.0);
  VertexOutput output;
  output.view_ray =
      mul(model_from_view, float4(mul(view_from_clip, vertex).xyz, 0.0)).xyz;
  output.position = vertex;
  return output;
}

/*
<h3>Pixel shader</h3>

<p>The pixel shader uses the following constants, as well as the following
atmosphere rendering functions. In GLSL these functions are defined externally,
by the <code>Model</code>'s shader, and the <code>USE_LUMINANCE</code> option
selects, with macros, either the functions returning radiance values, or those
returning luminance values. Here we define them as simple wrappers around the
functions of <a href="../atmosphere/atmosphere.hlsli.html">atmosphere.hlsli</a>,
which get the precomputed textures from the descriptor heap:
*/

static const float kLengthUnitInMeters = LENGTH_UNIT_IN_METERS;
static const float3 kSphereCenter =
    float3(0.0, 0.0, 1000.0) / kLengthUnitInMeters;
static const float kSphereRadius = 1000.0 / kLengthUnitInMeters;
static const float3 kSphereAlbedo = (float3)0.8;
static const float3 kGroundAlbedo = float3(0.0, 0.0, 0.04);

float3 GetSolarRadiance() {
#ifdef USE_LUMINANCE
  return GetSolarLuminance(atmosphere, sun_spectral_radiance_to_luminance);
#else
  return GetSolarRadiance(atmosphere);
#endif
}

float3 GetSkyRadiance(float3 camera, float3 view_ray, float shadow_length,
    float3 sun_direction, out float3 transmittance) {
  Texture2D<float4> transmittance_texture =
      ResourceDescriptorHeap[resources.transmittance_texture];
  Texture3D<float4> scattering_texture =
      ResourceDescriptorHeap[resources.scattering_texture];
  Texture3D<float4> single_mie_scattering_texture =
      ResourceDescriptorHeap[resources.single_mie_scattering_texture];
#ifdef USE_LUMINANCE
  return GetSkyLuminance(atmosphere, sky_spectral_radiance_to_luminance,
      transmittance_texture, scattering_texture, single_mie_scattering_texture,
      lut_sampler, camera, view_ray, shadow_length, sun_direction,
      transmittance);
#else
  return GetSkyRadiance(atmosphere, transmittance_texture, scattering_texture,
      single_mie_scattering_texture, lut_sampler, camera, view_ray,
      shadow_length, sun_direction, transmittance);
#endif
}

float3 GetSkyRadianceToPoint(float3 camera, float3 position,
    float shadow_length, float3 sun_direction, out float3 transmittance) {
  Texture2D<float4> transmittance_texture =
      ResourceDescriptorHeap[resources.transmittance_texture];
  Texture3D<float4> scattering_texture =
      ResourceDescriptorHeap[resources.scattering_texture];
  Texture3D<float4> single_mie_scattering_texture =
      ResourceDescriptorHeap[resources.single_mie_scattering_texture];
#ifdef USE_LUMINANCE
  return GetSkyLuminanceToPoint(atmosphere, sky_spectral_radiance_to_luminance,
      transmittance_texture, scattering_texture, single_mie_scattering_texture,
      lut_sampler, camera, position, shadow_length, sun_direction,
      transmittance);
#else
  return GetSkyRadianceToPoint(atmosphere, transmittance_texture,
      scattering_texture, single_mie_scattering_texture, lut_sampler, camera,
      position, shadow_length, sun_direction, transmittance);
#endif
}

float3 GetSunAndSkyIrradiance(float3 position, float3 normal,
    float3 sun_direction, out float3 sky_irradiance) {
  Texture2D<float4> transmittance_texture =
      ResourceDescriptorHeap[resources.transmittance_texture];
  Texture2D<float4> irradiance_texture =
      ResourceDescriptorHeap[resources.irradiance_texture];
#if defined(USE_LUMINANCE) && defined(SPECTRAL_SUN_TRANSMITTANCE)
  Texture2D<float4> sun_transmittance_texture =
      ResourceDescriptorHeap[resources.sun_transmittance_texture];
  return GetSpectralSunAndSkyIlluminance(atmosphere,
      sky_spectral_radiance_to_luminance, sun_spectral_radiance_to_luminance,
      sun_transmittance_texture, irradiance_texture, lut_sampler, position,
      normal, sun_direction, sky_irradiance);
#elif defined(USE_LUMINANCE)
  return GetSunAndSkyIlluminance(atmosphere,
      sky_spectral_radiance_to_luminance, sun_spectral_radiance_to_luminance,
      transmittance_texture, irradiance_texture, lut_sampler, position, normal,
      sun_direction, sky_irradiance);
#else
  return GetSunAndSkyIrradiance(atmosphere, transmittance_texture,
      irradiance_texture, lut_sampler, position, normal, sun_direction,
      sky_irradiance);
#endif
}

#if defined(USE_LUMINANCE) && defined(SPECTRAL_SUN_TRANSMITTANCE)
float3 GetSunTransmittance(float3 camera, float3 direction) {
  Texture2D<float4> sun_transmittance_texture =
      ResourceDescriptorHeap[resources.sun_transmittance_texture];
  return GetSunTransmittance(atmosphere, sun_transmittance_texture,
      lut_sampler, camera, direction);
}
#endif

/*<h3>Shadows and light shafts</h3>

<p>The functions to compute shadows and light shafts must be defined before we
can use them in the main shader function, so we define them first. Testing if
a point is in the shadow of the sphere S is equivalent to test if the
corresponding light ray intersects the sphere, which is very simple to do.
However, this is only valid for a punctual light source, which is not the case
of the Sun. In the following function we compute an approximate (and biased)
soft shadow by taking the angular size of the Sun into account:
*/

float GetSunVisibility(float3 position, float3 sun_direction) {
  float3 p = position - kSphereCenter;
  float p_dot_v = dot(p, sun_direction);
  float p_dot_p = dot(p, p);
  float ray_sphere_center_squared_distance = p_dot_p - p_dot_v * p_dot_v;
  float discriminant =
      kSphereRadius * kSphereRadius - ray_sphere_center_squared_distance;
  if (discriminant >= 0.0) {
    float distance_to_intersection = -p_dot_v - sqrt(discriminant);
    if (distance_to_intersection > 0.0) {
      // Compute the distance between the view ray and the sphere, and the
      // corresponding (tangent of the) subtended angle. Finally, use this to
      // compute an approximate sun visibility.
      float ray_sphere_distance =
          kSphereRadius - sqrt(ray_sphere_center_squared_distance);
      float ray_sphere_angular_distance = -ray_sphere_distance / p_dot_v;
      return smoothstep(1.0, 0.0, ray_sphere_angular_distance / sun_size.x);
    }
  }
  return 1.0;
}

/*
<p>The sphere also partially occludes the sky light, and we approximate this
effect with an ambient occlusion factor. The ambient occlusion factor due to a
sphere is given in <a href=
"http://webserver.dmt.upm.es/~isidoro/tc3/Radiation%20View%20factors.pdf"
>Radiation View Factors</a> (Isidoro Martinez, 1995). In the simple case where
the sphere is fully visible, it is given by the following function:
*/

float GetSkyVisibility(float3 position) {
  float3 p = position - kSphereCenter;
  float p_dot_p = dot(p, p);
  return
      1.0 + p.z / sqrt(p_dot_p) * kSphereRadius * kSphereRadius / p_dot_p;
}

/*
<p>To compute light shafts we need the intersections of the view ray with the
shadow volume of the sphere S. Since the Sun is not a punctual light source this
shadow volume is not a cylinder but a cone (for the umbra, plus another cone for
the penumbra, but we ignore it here):

<svg width="505px" height="200px">
  <style type="text/css"><![CDATA[
    circle { fill: #000000; stroke: none; }
    path { fill: none; stroke: #000000; }
    text { font-size: 16px; font-style: normal; font-family: Sans; }
    .vector { font-weight: bold; }
  ]]></style>
  <path d="m 10,75 455,120"/>
  <path d="m 10,125 455,-120"/>
  <path d="m 120,50 160,130"/>
  <path d="m 138,70 7,0 0,-7"/>
  <path d="m 410,65 40,0 m -5,-5 5,5 -5,5"/>
  <path d="m 20,100 430,0" style="stroke-dasharray:8,4,2,4;"/>
  <path d="m 255,25 0,155" style="stroke-dasharray:2,2;"/>
  <path d="m 280,160 -25,0" style="stroke-dasharray:2,2;"/>
  <path d="m 255,140 60,0" style="stroke-dasharray:2,2;"/>
  <path d="m 300,105 5,-5 5,5 m -5,-5 0,40 m -5,-5 5,5 5,-5"/>
  <path d="m 265,105 5,-5 5,5 m -5,-5 0,60 m -5,-5 5,5 5,-5"/>
  <path d="m 260,80 -5,5 5,5 m -5,-5 85,0 m -5,5 5,-5 -5,-5"/>
  <path d="m 335,95 5,5 5,-5 m -5,5 0,-60 m -5,5 5,-5 5,5"/>
  <path d="m 50,100 a 50,50 0 0 1 2,-14" style="stroke-dasharray:2,1;"/>
  <circle cx="340" cy="100" r="60" style="fill: none; stroke: #000000;"/>
  <circle cx="340" cy="100" r="2.5"/>
  <circle cx="255" cy="160" r="2.5"/>
  <circle cx="120" cy="50" r="2.5"/>
  <text x="105" y="45" class="vector">p</text>
  <text x="240" y="170" class="vector">q</text>
  <text x="425" y="55" class="vector">s</text>
  <text x="135" y="55" class="vector">v</text>
  <text x="345" y="75">R</text>
  <text x="275" y="135">r</text>
  <text x="310" y="125">ρ</text>
  <text x="215" y="120">d</text>
  <text x="290" y="80">δ</text>
  <text x="30" y="95">α</text>
</svg>

<p>Noting, as in the above figure, $\bp$ the camera position, $\bv$ and $\bs$
the unit view ray and sun direction vectors and $R$ the sphere radius (supposed
to be centered on the origin), the point at distance $d$ from the camera is
$\bq=\bp+d\bv$. This point is at a distance $\delta=-\bq\cdot\bs$ from the
sphere center along the umbra cone axis, and at a distance $r$ from this axis
given by $r^2=\bq\cdot\bq-\delta^2$. Finally, at distance $\delta$ along the
axis the umbra cone has radius $\rho=R-\delta\tan\alpha$, where $\alpha$ is
the Sun's angular radius. The point at distance $d$ from the camera is on the
shadow cone only if $r^2=\rho^2$, i.e. only if
\begin{equation}
(\bp+d\bv)\cdot(\bp+d\bv)-((\bp+d\bv)\cdot\bs)^2=
(R+((\bp+d\bv)\cdot\bs)\tan\alpha)^2
\end{equation}
Developping this gives a quadratic equation for $d$:
\begin{equation}
ad^2+2bd+c=0
\end{equation}
where
<ul>
<li>$a=1-l(\bv\cdot\bs)^2$,</li>
<li>$b=\bp\cdot\bv-l(\bp\cdot\bs)(\bv\cdot\bs)-\tan(\alpha)R(\bv\cdot\bs)$,</li>
<li>$c=\bp\cdot\bp-l(\bp\cdot\bs)^2-2\tan(\alpha)R(\bp\cdot\bs)-R^2$,</li>
<li>$l=1+\tan^2\alpha$</li>
</ul>
From this we deduce the two possible solutions for $d$, which must be clamped to
the actual shadow part of the mathematical cone (i.e. the slab between the
sphere center and the cone apex or, in other words, the points for which
$\delta$ is between $0$ and $R/\tan\alpha$). The following function implements
these equations:
*/

void GetSphereShadowInOut(float3 view_direction, float3 sun_direction,
    out float d_in, out float d_out) {
  float3 pos = camera - kSphereCenter;
  float pos_dot_sun = dot(pos, sun_direction);
  float view_dot_sun = dot(view_direction, sun_direction);
  float k = sun_size.x;
  float l = 1.0 + k * k;
  float a = 1.0 - l * view_dot_sun * view_dot_sun;
  float b = dot(pos, view_direction) - l * pos_dot_sun * view_dot_sun -
      k * kSphereRadius * view_dot_sun;
  float c = dot(pos, pos) - l * pos_dot_sun * pos_dot_sun -
      2.0 * k * kSphereRadius * pos_dot_sun - kSphereRadius * kSphereRadius;
  float discriminant = b * b - a * c;
  if (discriminant > 0.0) {
    d_in = max(0.0, (-b - sqrt(discriminant)) / a);
    d_out = (-b + sqrt(discriminant)) / a;
    // The values of d for which delta is equal to 0 and kSphereRadius / k.
    float d_base = -pos_dot_sun / view_dot_sun;
    float d_apex = -(pos_dot_sun + kSphereRadius / k) / view_dot_sun;
    if (view_dot_sun > 0.0) {
      d_in = max(d_in, d_apex);
      d_out = a > 0.0 ? min(d_out, d_base) : d_base;
    } else {
      d_in = a > 0.0 ? max(d_in, d_base) : d_base;
      d_out = min(d_out, d_apex);
    }
  } else {
    d_in = 0.0;
    d_out = 0.0;
  }
}

/*<h3>Tone mapping</h3>

<p>The final radiance (or luminance) values must be mapped to displayable
values. A simple per channel curve such as $1-\exp(-x)$ compresses each
channel independently, which shifts the hue of bright saturated colors (e.g.
the low Sun turns yellow then white, with a sharp split where its blue
component becomes 0). Instead, we use
<a href="https://github.com/sobotka/AgX">AgX</a>, which first mixes the color
channels with an "inset" matrix (so that very bright colors desaturate
smoothly towards white), then applies a sigmoid curve in log2 space (covering
16.5 stops around middle gray), and finally an "outset" matrix restoring some
of the chroma. The following code is a port of the AgX implementation of the
<a href="https://godotengine.org">Godot Engine</a> (version 4.4-stable,
<code>servers/rendering/renderer_rd/shaders/effects/tonemap.glsl</code>), which
is distributed under the license below (Godot's code is itself an
approximation of <a href="https://github.com/EaryChow/AgX_LUT_Gen">EaryChow's
AgX</a>, used by Blender). Note that AgX expects linear sRGB (Rec.709) inputs,
which is only approximately the case in radiance mode, where the RGB channels
are the spectral radiance at 680, 550 and 440nm (unless the Rec.709
band-averaged coefficients option is used). The Godot license is:

<pre>
Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md).
Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
</pre>

<p>The sigmoid curve is approximated with a polynomial (fitted by the Godot
authors to EaryChow's curve):
*/

float3 AgxContrastApprox(float3 x) {
  float3 x2 = x * x;
  float3 x4 = x2 * x2;
  return 0.021 * x + 4.0111 * x2 - 25.682 * x2 * x + 70.359 * x4 -
      74.778 * x4 * x + 27.069 * x4 * x2;
}

/*
<p>The AgX transform itself takes and returns linear sRGB values. Note that the
GLSL matrices are column major, and are used as <code>M * color</code>. We use
the same coefficients in the same order in HLSL, where they define the rows of
the matrices, with <code>mul(color, M)</code>, which gives the same result:
*/

float3 AgxToneMap(float3 color) {
  // Combined linear sRGB to linear Rec 2020 and Blender AgX inset matrices.
  static const float3x3 kSrgbToRec2020AgxInset = float3x3(
      0.54490813676363087053, 0.14044005884001287035, 0.088827411851915368603,
      0.37377945959812267119, 0.75410959864013760045, 0.17887712465043811023,
      0.081384976686407536266, 0.10543358536857773485, 0.73224999956948382528);
  // Combined inverse AgX outset and linear Rec 2020 to linear sRGB matrices.
  static const float3x3 kAgxOutsetRec2020ToSrgb = float3x3(
      1.9645509602733325934, -0.29932243390911083839, -0.16436833806080403409,
      -0.85585845117807513559, 1.3264510741502356555, -0.23822464068860595117,
      -0.10886710826831608324, -0.027084020983874825605, 1.402665347143271889);
  // log2(pow(2, -10) * 0.18) and log2(pow(2, 6.5) * 0.18), i.e. 10 stops
  // below and 6.5 stops above middle gray.
  static const float kMinEv = -12.4739311883324;
  static const float kMaxEv = 4.02606881166759;

  // Negative inputs are not supported (and 2e-10 avoids log2(0) after the
  // inset matrix).
  color = max(color, 2e-10);
  color = mul(color, kSrgbToRec2020AgxInset);
  // Log2 encoding, clamped to [0,1] for the polynomial approximation.
  color = (clamp(log2(color), kMinEv, kMaxEv) - kMinEv) / (kMaxEv - kMinEv);
  color = AgxContrastApprox(color);
  // Back to linear, then outset matrix and conversion back to linear sRGB.
  color = pow(color, 2.4);
  return mul(color, kAgxOutsetRec2020ToSrgb);
}

/*
<p>The result, clamped to [0,1], is then encoded with the sRGB transfer
function (the swap chain uses a UNORM format):
*/

float3 LinearToSrgb(float3 color) {
  color = saturate(color);
  return select(color <= 0.0031308,
      12.92 * color, 1.055 * pow(color, 1.0 / 2.4) - 0.055);
}

/*
<h3>Film emulation LUT</h3>

<p>Optionally, AgX can be replaced with a more "aesthetic" tone mapping, defined
by a 3D lookup table (LUT) baked from a film emulation model (see
<code>data/README.md</code>). This LUT takes linear Rec.709 values (with middle
gray at 0.18, as for AgX), and directly returns the final display values,
already encoded with the sRGB transfer function. Its 65 samples per channel are
not spaced linearly but logarithmically: the input values are first encoded
with a "shaper" function, which covers 8 stops below and above middle gray in
log2 space, extended below this range with a linear "toe" through the origin
(tangent to the log2 curve, so that 0 maps to 0). The constants below must be
the ones used to bake the LUT:
*/

static const float kFilmLutSize = 65.0;
static const float kFilmLutMidGray = 0.18;
static const float kFilmLutMinStops = -8.0;
static const float kFilmLutMaxStops = 8.0;

float3 FilmLutShaper(float3 color) {
  const float kRange = kFilmLutMaxStops - kFilmLutMinStops;
  // The toe ends 1/ln(2) stops above the minimum of the log2 range, where the
  // tangent of the log2 curve passes through the origin.
  const float kToeEnd =
      kFilmLutMidGray * exp2(kFilmLutMinStops + 1.0 / log(2.0));
  const float kToeSlope = 1.0 / (kToeEnd * log(2.0) * kRange);
  // Negative values and NaNs are mapped to 0.
  color = max(color, 0.0);
  float3 log_code = (log2(max(color, kToeEnd)) -
      (log2(kFilmLutMidGray) + kFilmLutMinStops)) / kRange;
  return saturate(select(color < kToeEnd, color * kToeSlope, log_code));
}

/*
<p>The LUT is sampled with a tetrahedral interpolation, instead of the
trilinear interpolation of the texture sampler. For this, the LUT cell
containing the shaped value is split into 6 tetrahedra which all share the main
diagonal between the cell corners $c_{000}$ and $c_{111}$. With the fractional
coordinates in the cell sorted as $f_{max}\ge f_{mid}\ge f_{min}$, the result
is $(1-f_{max})c_{000}+(f_{max}-f_{mid})c_1+(f_{mid}-f_{min})c_2+f_{min}
c_{111}$, where $c_1$ is the corner along the axis of $f_{max}$, and $c_2$ the
corner along the axes of $f_{max}$ and $f_{mid}$. This needs only 4 texel
loads, uses full precision weights, and the neutral colors (with equal
fractional coordinates) only depend on the neutral samples of the LUT:
*/

float3 SampleFilmLut(Texture3D<float4> lut, float3 code) {
  float3 p = saturate(code) * (kFilmLutSize - 1.0);
  float3 base = min(floor(p), kFilmLutSize - 2.0);
  float3 f = p - base;

  bool x_ge_y = f.x >= f.y;
  bool y_ge_z = f.y >= f.z;
  bool x_ge_z = f.x >= f.z;
  // Axis of the largest fraction (ties prefer x, then y).
  int3 o1 = (x_ge_y && x_ge_z) ? int3(1, 0, 0) :
      (y_ge_z ? int3(0, 1, 0) : int3(0, 0, 1));
  // Axis of the smallest fraction (ties prefer z, then y), never equal to o1.
  int3 o_min = (x_ge_z && y_ge_z) ? int3(0, 0, 1) :
      (x_ge_y ? int3(0, 1, 0) : int3(1, 0, 0));
  int3 o2 = int3(1, 1, 1) - o_min;

  float f_max = max(f.x, max(f.y, f.z));
  float f_min = min(f.x, min(f.y, f.z));
  float f_mid = max(min(f.x, f.y), min(max(f.x, f.y), f.z));

  int3 i0 = int3(base);
  float3 c0 = lut.Load(int4(i0, 0)).rgb;
  float3 c1 = lut.Load(int4(i0 + o1, 0)).rgb;
  float3 c2 = lut.Load(int4(i0 + o2, 0)).rgb;
  float3 c3 = lut.Load(int4(i0 + int3(1, 1, 1), 0)).rgb;
  return c0 * (1.0 - f_max) + c1 * (f_max - f_mid) + c2 * (f_mid - f_min) +
      c3 * f_min;
}

/*
<p>Finally, the following function applies the selected tone mapping, and
returns sRGB encoded display values:
*/

float3 ToneMap(float3 color) {
  if (resources.tone_mapping != 0) {
    Texture3D<float4> film_lut =
        ResourceDescriptorHeap[resources.film_lut_texture];
    return saturate(SampleFilmLut(film_lut, FilmLutShaper(color)));
  }
  return LinearToSrgb(AgxToneMap(color));
}

/*<h3>Main shading function</h3>

<p>Using these functions we can now implement the main shader function, which
computes the radiance from the scene for a given view ray. This function first
tests if the view ray intersects the sphere S. If so it computes the sun and
sky light received by the sphere at the intersection point, combines this with
the sphere BRDF and the aerial perspective between the camera and the sphere.
It then does the same with the ground, i.e. with the planet sphere P, and then
computes the sky radiance and transmittance. Finally, all these terms are
composited together (an opacity is also computed for each object, using an
approximate view cone - sphere intersection factor) to get the final radiance.

<p>We start with the computation of the intersections of the view ray with the
shadow volume of the sphere, because they are needed to get the aerial
perspective for the sphere and the planet:
*/

float4 DemoPS(VertexOutput input) : SV_Target {
  float3 view_ray = input.view_ray;
  // Normalized view direction vector.
  float3 view_direction = normalize(view_ray);
  // Tangent of the angle subtended by this fragment (note that ddy is equal to
  // -dFdy in GLSL, because the y axis of the screen points down in D3D, and up
  // in OpenGL).
  float fragment_angular_size =
      length(ddx(view_ray) - ddy(view_ray)) / length(view_ray);

  float shadow_in;
  float shadow_out;
  GetSphereShadowInOut(view_direction, sun_direction, shadow_in, shadow_out);

  // Hack to fade out light shafts when the Sun is very close to the horizon.
  float lightshaft_fadein_hack = smoothstep(
      0.02, 0.04, dot(normalize(camera - earth_center), sun_direction));

/*
<p>We then test whether the view ray intersects the sphere S or not. If it does,
we compute an approximate (and biased) opacity value, using the same
approximation as in <code>GetSunVisibility</code>:
*/

  // Compute the distance between the view ray line and the sphere center,
  // and the distance between the camera and the intersection of the view
  // ray with the sphere (or NaN if there is no intersection).
  float3 p = camera - kSphereCenter;
  float p_dot_v = dot(p, view_direction);
  float p_dot_p = dot(p, p);
  float ray_sphere_center_squared_distance = p_dot_p - p_dot_v * p_dot_v;
  float discriminant =
      kSphereRadius * kSphereRadius -ray_sphere_center_squared_distance;

  // Compute the radiance reflected by the sphere, if the ray intersects it.
  float sphere_alpha = 0.0;
  float3 sphere_radiance = (float3)0.0;
  if (discriminant >= 0.0) {
    float distance_to_intersection = -p_dot_v - sqrt(discriminant);
    if (distance_to_intersection > 0.0) {
      // Compute the distance between the view ray and the sphere, and the
      // corresponding (tangent of the) subtended angle. Finally, use this to
      // compute the approximate analytic antialiasing factor sphere_alpha.
      float ray_sphere_distance =
          kSphereRadius - sqrt(ray_sphere_center_squared_distance);
      float ray_sphere_angular_distance = -ray_sphere_distance / p_dot_v;
      sphere_alpha =
          min(ray_sphere_angular_distance / fragment_angular_size, 1.0);

/*
<p>We can then compute the intersection point and its normal, and use them to
get the sun and sky irradiance received at this point. The reflected radiance
follows, by multiplying the irradiance with the sphere BRDF:
*/
      float3 position = camera + view_direction * distance_to_intersection;
      float3 normal = normalize(position - kSphereCenter);

      // Compute the radiance reflected by the sphere.
      float3 sky_irradiance;
      float3 sun_irradiance = GetSunAndSkyIrradiance(
          position - earth_center, normal, sun_direction, sky_irradiance);
      sphere_radiance =
          kSphereAlbedo * (1.0 / PI) * (sun_irradiance + sky_irradiance);

/*
<p>Finally, we take into account the aerial perspective between the camera and
the sphere, which depends on the length of this segment which is in shadow:
*/
      float shadow_length =
          max(0.0, min(shadow_out, distance_to_intersection) - shadow_in) *
          lightshaft_fadein_hack;
      float3 transmittance;
      float3 in_scatter = GetSkyRadianceToPoint(camera - earth_center,
          position - earth_center, shadow_length, sun_direction, transmittance);
      sphere_radiance = sphere_radiance * transmittance + in_scatter;
    }
  }

/*
<p>In the following we repeat the same steps as above, but for the planet sphere
P instead of the sphere S (a smooth opacity is not really needed here, so we
don't compute it. Note also how we modulate the sun and sky irradiance received
on the ground by the sun and sky visibility factors):
*/

  // Compute the distance between the view ray line and the Earth center,
  // and the distance between the camera and the intersection of the view
  // ray with the ground (or NaN if there is no intersection).
  p = camera - earth_center;
  p_dot_v = dot(p, view_direction);
  p_dot_p = dot(p, p);
  float ray_earth_center_squared_distance = p_dot_p - p_dot_v * p_dot_v;
  discriminant =
      earth_center.z * earth_center.z - ray_earth_center_squared_distance;

  // Compute the radiance reflected by the ground, if the ray intersects it.
  float ground_alpha = 0.0;
  float3 ground_radiance = (float3)0.0;
  if (discriminant >= 0.0) {
    float distance_to_intersection = -p_dot_v - sqrt(discriminant);
    if (distance_to_intersection > 0.0) {
      float3 position = camera + view_direction * distance_to_intersection;
      float3 normal = normalize(position - earth_center);

      // Compute the radiance reflected by the ground.
      float3 sky_irradiance;
      float3 sun_irradiance = GetSunAndSkyIrradiance(
          position - earth_center, normal, sun_direction, sky_irradiance);
      ground_radiance = kGroundAlbedo * (1.0 / PI) * (
          sun_irradiance * GetSunVisibility(position, sun_direction) +
          sky_irradiance * GetSkyVisibility(position));

      float shadow_length =
          max(0.0, min(shadow_out, distance_to_intersection) - shadow_in) *
          lightshaft_fadein_hack;
      float3 transmittance;
      float3 in_scatter = GetSkyRadianceToPoint(camera - earth_center,
          position - earth_center, shadow_length, sun_direction, transmittance);
      ground_radiance = ground_radiance * transmittance + in_scatter;
      ground_alpha = 1.0;
    }
  }

/*
<p>Finally, we compute the radiance and transmittance of the sky, and composite
together, from back to front, the radiance and opacities of all the objects of
the scene:
*/

  // Compute the radiance of the sky.
  float shadow_length = max(0.0, shadow_out - shadow_in) *
      lightshaft_fadein_hack;
  float3 transmittance;
  float3 radiance = GetSkyRadiance(
      camera - earth_center, view_direction, shadow_length, sun_direction,
      transmittance);

  // If the view ray intersects the Sun, add the Sun radiance (attenuated with
  // the spectral sun transmittance, if available, which gives its exact color,
  // instead of the transmittance of the view ray).
  if (dot(view_direction, sun_direction) > sun_size.y) {
#if defined(USE_LUMINANCE) && defined(SPECTRAL_SUN_TRANSMITTANCE)
    transmittance = GetSunTransmittance(camera - earth_center, view_direction);
#endif
    radiance = radiance + transmittance * GetSolarRadiance();
  }
  radiance = lerp(radiance, ground_radiance, ground_alpha);
  radiance = lerp(radiance, sphere_radiance, sphere_alpha);
  // With the default exposure and the AgX tone mapping, this gives about the
  // same mean display luminance as the 1 - exp(-x) curve of the OpenGL demo
  // (0.93 to 1.18 times over the predefined views), with more contrast.
  return float4(ToneMap(radiance / white_point * exposure), 1.0);
}
