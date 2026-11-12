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

/*<h2>shaders/atmosphere/definitions.hlsli</h2>

<p>This HLSL file defines the constants and the parameter structs which are used
in the main <a href="functions.hlsli.html">functions</a> of our atmosphere
model. It is a port of the GLSL <code>atmosphere/definitions.glsl</code> file.

<p>The GLSL version defines a type alias for each physical quantity
(<code>Length</code>, <code>IrradianceSpectrum</code>, etc), in order to share
the same code with a C++ version where these types are checked at compile time
for dimensional homogeneity. This HLSL port is not compiled as C++, and simply
uses <code>float</code> for scalar quantities and <code>float3</code> for
vectors of quantities - including functions of the wavelength, whose 3
components correspond to the values of the function at 3 predefined
wavelengths. The comments in the code below give the units of these values.

<h3>Texture sizes</h3>

<p>The precomputed textures have fixed sizes. They are defined here as
preprocessor macros, so that they can be overridden (with compiler
<code>-D</code> options) by the C++ code doing the precomputations, which
then remains the single source of truth for these values:
*/

#ifndef ATMOSPHERE_DEFINITIONS_HLSLI_
#define ATMOSPHERE_DEFINITIONS_HLSLI_

#ifndef TRANSMITTANCE_TEXTURE_WIDTH
#define TRANSMITTANCE_TEXTURE_WIDTH 256
#endif
#ifndef TRANSMITTANCE_TEXTURE_HEIGHT
#define TRANSMITTANCE_TEXTURE_HEIGHT 64
#endif

#ifndef SCATTERING_TEXTURE_R_SIZE
#define SCATTERING_TEXTURE_R_SIZE 32
#endif
#ifndef SCATTERING_TEXTURE_MU_SIZE
#define SCATTERING_TEXTURE_MU_SIZE 128
#endif
#ifndef SCATTERING_TEXTURE_MU_S_SIZE
#define SCATTERING_TEXTURE_MU_S_SIZE 32
#endif
#ifndef SCATTERING_TEXTURE_NU_SIZE
#define SCATTERING_TEXTURE_NU_SIZE 8
#endif

#ifndef IRRADIANCE_TEXTURE_WIDTH
#define IRRADIANCE_TEXTURE_WIDTH 64
#endif
#ifndef IRRADIANCE_TEXTURE_HEIGHT
#define IRRADIANCE_TEXTURE_HEIGHT 16
#endif

/*
<p>The 4D scattering function is stored in 3D textures, whose sizes are derived
from the above values:
*/

#define SCATTERING_TEXTURE_WIDTH \
    (SCATTERING_TEXTURE_NU_SIZE * SCATTERING_TEXTURE_MU_S_SIZE)
#define SCATTERING_TEXTURE_HEIGHT SCATTERING_TEXTURE_MU_SIZE
#define SCATTERING_TEXTURE_DEPTH SCATTERING_TEXTURE_R_SIZE

/*
<h3>Physical units</h3>

<p>We define the units for our six base physical quantities: meter (m),
nanometer (nm), radian (rad), steradian (sr), watt (watt) and lumen (lm). With
plain <code>float</code> values these are all equal to 1, but using them in
expressions such as <code>0.0 * m</code> keeps the code self-documenting, and
identical to the original GLSL code:
*/

static const float m = 1.0;
static const float nm = 1.0;
static const float rad = 1.0;
static const float sr = 1.0;
static const float watt = 1.0;
static const float lm = 1.0;

/*
<p>From which we can derive the units for some derived physical quantities,
as well as some derived units (kilometer km, kilocandela kcd, degree deg):
*/

static const float PI = 3.14159265358979323846;

static const float km = 1000.0 * m;
static const float m2 = m * m;
static const float m3 = m * m * m;
static const float pi = PI * rad;
static const float deg = pi / 180.0;
static const float watt_per_square_meter = watt / m2;
static const float watt_per_square_meter_per_sr = watt / (m2 * sr);
static const float watt_per_square_meter_per_nm = watt / (m2 * nm);
static const float watt_per_square_meter_per_sr_per_nm =
    watt / (m2 * sr * nm);
static const float watt_per_cubic_meter_per_sr_per_nm =
    watt / (m3 * sr * nm);
static const float cd = lm / sr;
static const float kcd = 1000.0 * cd;
static const float cd_per_square_meter = cd / m2;
static const float kcd_per_square_meter = kcd / m2;

/*
<h3>Atmosphere parameters</h3>

<p>We can now define the parameters of our atmosphere model. We start with the
definition of density profiles, which are needed for parameters that depend on
the altitude:
*/

// An atmosphere layer of width 'width' (a length), and whose density is
// defined as
//   'exp_term' * exp('exp_scale' * h) + 'linear_term' * h + 'constant_term',
// clamped to [0,1], and where h is the altitude. 'exp_term' and
// 'constant_term' are unitless, while 'exp_scale' and 'linear_term' are
// inverse lengths.
struct DensityProfileLayer {
  float width;
  float exp_term;
  float exp_scale;
  float linear_term;
  float constant_term;
};

// An atmosphere density profile made of several layers on top of each other
// (from bottom to top). The width of the last layer is ignored, i.e. it always
// extend to the top atmosphere boundary. The profile values vary between 0
// (null density) to 1 (maximum density).
struct DensityProfile {
  DensityProfileLayer layers[2];
};

/*
<p>The atmosphere parameters are then defined by the following struct. Note
that, unlike in the GLSL version where it is a compile time constant, this
struct is meant to be stored in a constant buffer, filled by the C++ code. When
doing so, beware of the HLSL constant buffer packing rules: each struct, and
each array element, starts on a 16 bytes boundary (e.g. the two
<code>DensityProfileLayer</code> of a <code>DensityProfile</code> are 32 bytes
apart, and not 20).
*/

struct AtmosphereParameters {
  // The solar irradiance at the top of the atmosphere (W.m^-2.nm^-1).
  float3 solar_irradiance;
  // The sun's angular radius (rad). Warning: the implementation uses
  // approximations that are valid only if this angle is smaller than 0.1
  // radians.
  float sun_angular_radius;
  // The distance between the planet center and the bottom of the atmosphere.
  float bottom_radius;
  // The distance between the planet center and the top of the atmosphere.
  float top_radius;
  // The density profile of air molecules, i.e. a function from altitude to
  // dimensionless values between 0 (null density) and 1 (maximum density).
  DensityProfile rayleigh_density;
  // The scattering coefficient of air molecules at the altitude where their
  // density is maximum (usually the bottom of the atmosphere), as a function of
  // wavelength (inverse length). The scattering coefficient at altitude h is
  // equal to 'rayleigh_scattering' times 'rayleigh_density' at this altitude.
  float3 rayleigh_scattering;
  // The density profile of aerosols, i.e. a function from altitude to
  // dimensionless values between 0 (null density) and 1 (maximum density).
  DensityProfile mie_density;
  // The scattering coefficient of aerosols at the altitude where their density
  // is maximum (usually the bottom of the atmosphere), as a function of
  // wavelength (inverse length). The scattering coefficient at altitude h is
  // equal to 'mie_scattering' times 'mie_density' at this altitude.
  float3 mie_scattering;
  // The extinction coefficient of aerosols at the altitude where their density
  // is maximum (usually the bottom of the atmosphere), as a function of
  // wavelength (inverse length). The extinction coefficient at altitude h is
  // equal to 'mie_extinction' times 'mie_density' at this altitude.
  float3 mie_extinction;
  // The asymetry parameter for the Cornette-Shanks phase function for the
  // aerosols.
  float mie_phase_function_g;
  // The density profile of air molecules that absorb light (e.g. ozone), i.e.
  // a function from altitude to dimensionless values between 0 (null density)
  // and 1 (maximum density).
  DensityProfile absorption_density;
  // The extinction coefficient of molecules that absorb light (e.g. ozone) at
  // the altitude where their density is maximum, as a function of wavelength
  // (inverse length). The extinction coefficient at altitude h is equal to
  // 'absorption_extinction' times 'absorption_density' at this altitude.
  float3 absorption_extinction;
  // The average albedo of the ground (unitless).
  float3 ground_albedo;
  // The cosine of the maximum Sun zenith angle for which atmospheric scattering
  // must be precomputed (for maximum precision, use the smallest Sun zenith
  // angle yielding negligible sky light radiance values. For instance, for the
  // Earth case, 102 degrees is a good choice - yielding mu_s_min = -0.2).
  float mu_s_min;
};

#endif  // ATMOSPHERE_DEFINITIONS_HLSLI_
