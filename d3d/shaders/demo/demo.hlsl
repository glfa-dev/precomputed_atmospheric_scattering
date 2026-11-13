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
    "RootConstants(num32BitConstants=5, b1)," \
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
  return float4(
      pow((float3)1.0 - exp(-radiance / white_point * exposure), 1.0 / 2.2),
      1.0);
}
