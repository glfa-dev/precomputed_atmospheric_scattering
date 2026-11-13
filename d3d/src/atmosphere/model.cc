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

#include <algorithm>
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
<p>In precomputed illuminance mode, the precomputed textures contain, for each
linear sRGB channel $c$, the integral $\int L(\lambda)\bar{c}(\lambda)d\lambda$
of the spectral radiance $L$ times the sRGB color matching function $\bar{c}$
(i.e. <code>XYZ_TO_SRGB</code> times the CIE color matching functions, without
the <code>MAX_LUMINOUS_EFFICACY</code> factor, see the
<a href="#implementation">Model constructor</a>). This
integral is approximated with a weighted sum $\sum_i w_{c,i}L(\lambda_i)$ of
the spectral radiance at a few wavelengths $\lambda_i$, each requiring a
precomputation (3 wavelengths per precomputation pass). The OpenGL version uses
the midpoint rule, i.e. evenly spaced $\lambda_i$ between 360 and 830nm, and
$w_{c,i}=\bar{c}(\lambda_i)\Delta\lambda$. This rule needs many wavelengths to
get accurate colors: the color matching functions have narrow peaks, and it
uses no information about the possible radiance spectra.

<p>But these spectra are not arbitrary: they are the solar spectrum, filtered
by the transmittance of the atmosphere along some paths, and by some scattering
events, with spectral variations entirely determined by the
<code>Model</code>'s input spectra. This can be used to get a much better
integration rule: for given wavelengths $\lambda_i$, we choose the weights
$w_{c,i}$ minimizing the integration error over a family of such "training"
spectra $f$, i.e. $\sum_f(\sum_i w_{c,i}f(\lambda_i)-\int
f\bar{c}d\lambda)^2$. This is a linear least squares problem, whose solution
depends on the input spectra (in particular, weights fitted for a given solar
spectrum give large errors with another one). Thus, we fit these weights here,
at construction time. The wavelengths themselves, on the other hand, were
optimized offline (by minimizing the residual error of this fit with a global
optimizer, for the default Earth atmosphere), and are fixed:
*/

constexpr double kFitted6Wavelengths[6] = {
  413.8688135933362, 447.3719319909487, 482.9039304243995,
  537.0640314119139, 592.8834954527024, 644.0572502549226
};
constexpr double kFitted9Wavelengths[9] = {
  412.37622700242605, 444.1180920345416, 458.2532482631017,
  491.25498775791857, 528.1929811255243, 570.3219845576455,
  578.5619239716891, 613.0124906093698, 660.2580585325449
};

/*
<p>The training spectra are of the form
$$f(\lambda)=S(\lambda)\left(\frac{\beta_R(\lambda)}{\beta_R(550)}\right)^a
\exp\left(-m_R V_R\beta_R(\lambda)-m_M m_R V_M\beta_M^{ext}(\lambda)-
m_O V_O\beta_O^{ext}(\lambda)\right)$$
where $S$ is the solar irradiance, $\beta_R$ the Rayleigh scattering
coefficient, $\beta_M^{ext}$ the Mie extinction coefficient, $\beta_O^{ext}$
the absorption extinction coefficient, and $V_R,V_M,V_O$ the vertical column
integrals of the corresponding density profiles (from the bottom to the top of
the atmosphere). In other words, these are solar spectra transmitted through
$m_R$ vertical air masses of molecules (from 0, i.e. no attenuation, to 38,
i.e. a horizontal path), with $m_M\in\{0,1,2.5\}$ times as many aerosols,
and $m_O$ vertical columns of ozone (up to 20, for long twilight paths through
the ozone layer), and "blued" by $a\in\{0,1,2,3\}$ Rayleigh scattering events
(0 for direct sunlight, 1 for single scattering, etc). They don't depend on the
view and sun directions, and are built only from the <code>Model</code> inputs.

<p>We first need the vertical column integrals of the density profiles, which
we compute with the trapezoidal rule, using the same layer selection and
clamping as the <code>GetProfileDensity</code> shader function (in <a href=
"../../shaders/atmosphere/functions.hlsli.html">functions.hlsli</a>):
*/

// Returns the integral of the given density profile between the bottom and the
// top of the atmosphere, in m.
double ComputeVerticalColumn(std::vector<DensityProfileLayer> layers,
                             double bottom_radius, double top_radius) {
  // Same padding as in the Model constructor: missing layers are added at the
  // bottom, and only the first 2 layers are used.
  while (layers.size() < 2) {
    layers.insert(layers.begin(), DensityProfileLayer());
  }
  auto density = [&layers](double altitude) {
    const DensityProfileLayer& layer =
        altitude < layers[0].width ? layers[0] : layers[1];
    double value = layer.exp_term * exp(layer.exp_scale * altitude) +
        layer.linear_term * altitude + layer.constant_term;
    return std::clamp(value, 0.0, 1.0);
  };
  constexpr int kSampleCount = 10000;
  const double dh = (top_radius - bottom_radius) / kSampleCount;
  double result = 0.0;
  for (int i = 0; i <= kSampleCount; ++i) {
    double weight = i == 0 || i == kSampleCount ? 0.5 : 1.0;
    result += density(i * dh) * weight * dh;
  }
  return result;
}

/*
<p>The weights are then fitted as follows. Each training spectrum $f$ is
tabulated with a 1nm step between 360 and 829nm (by linear interpolation of
the input spectra), which gives its exact integral with the sRGB color matching
functions (with the same 1nm step as in
<code>ConvertSpectrumToLinearSrgb</code>), and its values at the $\lambda_i$
(by linear interpolation of this 1nm table). Each spectrum is normalized by its
luminance $Y$ (computed from its exact linear sRGB value), so that the fit
minimizes relative errors, which are those that are visible after exposure
adjustment, and the spectra with a negligible luminance (e.g. through 38 air
masses and 20 ozone columns) are dropped. Finally, to avoid ill-conditioned
solutions with very large weights of opposite signs, we add a small ridge
regularization term $\epsilon\Vert w_c\Vert^2$ to the least squares error, and
we solve the corresponding normal equations with a Cholesky decomposition:
*/

// The signed linear sRGB color matching function for the given channel,
// without the MAX_LUMINOUS_EFFICACY factor.
double SrgbColorMatchingFunction(double wavelength, int component) {
  double x = CieColorMatchingFunctionTableValue(wavelength, 1);
  double y = CieColorMatchingFunctionTableValue(wavelength, 2);
  double z = CieColorMatchingFunctionTableValue(wavelength, 3);
  return XYZ_TO_SRGB[component * 3] * x + XYZ_TO_SRGB[component * 3 + 1] * y +
      XYZ_TO_SRGB[component * 3 + 2] * z;
}

// Solves the symmetric positive definite n x n system 'a' x = 'b' (row major
// matrix), with a Cholesky decomposition (computed in place in the local copy
// of 'a').
std::vector<double> SolveCholesky(std::vector<double> a,
                                  const std::vector<double>& b) {
  const size_t n = b.size();
  for (size_t j = 0; j < n; ++j) {
    for (size_t k = 0; k < j; ++k) {
      a[j * n + j] -= a[j * n + k] * a[j * n + k];
    }
    // The ridge term makes the matrix positive definite, so this floor is
    // only a guard against a division by zero (e.g. with NaN inputs).
    a[j * n + j] = sqrt(std::max(a[j * n + j], 1e-300));
    for (size_t i = j + 1; i < n; ++i) {
      for (size_t k = 0; k < j; ++k) {
        a[i * n + j] -= a[i * n + k] * a[j * n + k];
      }
      a[i * n + j] /= a[j * n + j];
    }
  }
  // Forward substitution (L y = b), then back substitution (L^T x = y).
  std::vector<double> x = b;
  for (size_t i = 0; i < n; ++i) {
    for (size_t k = 0; k < i; ++k) {
      x[i] -= a[i * n + k] * x[k];
    }
    x[i] /= a[i * n + i];
  }
  for (size_t i = n; i-- > 0;) {
    for (size_t k = i + 1; k < n; ++k) {
      x[i] -= a[k * n + i] * x[k];
    }
    x[i] /= a[i * n + i];
  }
  return x;
}

// Returns the weights w[i][c] of the wavelengths 'nodes' for the linear sRGB
// channel c, in the same unit as the luminance_from_radiance coefficients of
// the midpoint rule (see the Model constructor). The other arguments are the
// Model inputs (in m^-1 for the scattering and extinction coefficients) and
// the vertical column integrals of the Rayleigh, Mie and absorption density
// profiles (in m). Returns an empty vector if the weights can't be fitted.
std::vector<std::array<double, 3>> FitWavelengthRuleWeights(
    const std::vector<double>& nodes,
    const std::vector<double>& wavelengths,
    const std::vector<double>& solar_irradiance,
    const std::vector<double>& rayleigh_scattering,
    const std::vector<double>& mie_extinction,
    const std::vector<double>& absorption_extinction,
    double rayleigh_column, double mie_column, double absorption_column) {
  // The input spectra and the sRGB color matching functions, tabulated with a
  // 1nm step from kLambdaMin to kLambdaMax - 1.
  constexpr int kSize = kLambdaMax - kLambdaMin;
  std::vector<double> solar(kSize), rayleigh(kSize), mie(kSize),
      absorption(kSize);
  std::vector<std::array<double, 3>> cmf(kSize);
  for (int j = 0; j < kSize; ++j) {
    double lambda = kLambdaMin + j;
    solar[j] = Interpolate(wavelengths, solar_irradiance, lambda);
    rayleigh[j] = Interpolate(wavelengths, rayleigh_scattering, lambda);
    mie[j] = Interpolate(wavelengths, mie_extinction, lambda);
    absorption[j] = Interpolate(wavelengths, absorption_extinction, lambda);
    for (int c = 0; c < 3; ++c) {
      cmf[j][c] = SrgbColorMatchingFunction(lambda, c);
    }
  }
  const double rayleigh_550 = rayleigh[550 - kLambdaMin];

  // The 1nm table indices and interpolation factors for each node.
  const size_t n = nodes.size();
  std::vector<int> node_index(n);
  std::vector<double> node_u(n);
  for (size_t i = 0; i < n; ++i) {
    double x = std::clamp(nodes[i], double(kLambdaMin), kLambdaMax - 1.0) -
        kLambdaMin;
    node_index[i] = std::min(static_cast<int>(std::floor(x)), kSize - 2);
    node_u[i] = x - node_index[i];
  }

  // The training spectra values at the nodes, their linear sRGB values (the
  // fit targets), and their luminance.
  struct Sample {
    std::vector<double> values;
    std::array<double, 3> rgb;
    double luminance;
  };
  std::vector<Sample> samples;
  std::vector<double> f(kSize);
  for (int a = 0; a <= 3; ++a) {
    for (double m_r : {0.0, 0.5, 1.0, 2.0, 4.0, 8.0, 12.0, 16.0, 24.0, 38.0}) {
      for (double m_m : {0.0, 1.0, 2.5}) {
        for (double m_o : {0.0, 1.0, 2.0, 4.0, 8.0, 12.0, 20.0}) {
          Sample sample = {std::vector<double>(n), {0.0, 0.0, 0.0}, 0.0};
          for (int j = 0; j < kSize; ++j) {
            double ratio =
                rayleigh_550 > 0.0 ? rayleigh[j] / rayleigh_550 : 1.0;
            double optical_depth = m_r * rayleigh_column * rayleigh[j] +
                m_m * m_r * mie_column * mie[j] +
                m_o * absorption_column * absorption[j];
            f[j] = solar[j] * pow(ratio, a) * exp(-optical_depth);
            for (int c = 0; c < 3; ++c) {
              sample.rgb[c] += cmf[j][c] * f[j];
            }
          }
          for (size_t i = 0; i < n; ++i) {
            int j = node_index[i];
            sample.values[i] =
                f[j] * (1.0 - node_u[i]) + f[j + 1] * node_u[i];
          }
          sample.luminance = 0.2126 * sample.rgb[0] +
              0.7152 * sample.rgb[1] + 0.0722 * sample.rgb[2];
          samples.push_back(std::move(sample));
        }
      }
    }
  }
  double max_luminance = 0.0;
  for (const Sample& sample : samples) {
    max_luminance = std::max(max_luminance, sample.luminance);
  }

  // The normal equations of the regularized least squares problem, with the
  // spectra normalized by their luminance: (A + epsilon I) w_c = b_c, with
  // A = sum_f v v^T and b_c = sum_f v rgb_c, where v are the values at the
  // nodes. As in the offline optimization of the nodes, epsilon is 1e-7 times
  // the average diagonal element of A.
  std::vector<double> normal_matrix(n * n, 0.0);
  std::vector<std::vector<double>> rhs(3, std::vector<double>(n, 0.0));
  for (const Sample& sample : samples) {
    if (!(sample.luminance > 1e-6 * max_luminance)) {
      continue;
    }
    const double scale = 1.0 / sample.luminance;
    for (size_t i = 0; i < n; ++i) {
      double v_i = sample.values[i] * scale;
      for (size_t k = 0; k < n; ++k) {
        normal_matrix[i * n + k] += v_i * sample.values[k] * scale;
      }
      for (int c = 0; c < 3; ++c) {
        rhs[c][i] += v_i * sample.rgb[c] * scale;
      }
    }
  }
  double trace = 0.0;
  for (size_t i = 0; i < n; ++i) {
    trace += normal_matrix[i * n + i];
  }
  if (!(trace > 0.0)) {
    // Degenerate case (e.g. a null or negative solar spectrum): no weights
    // can be fitted, and the caller falls back to the midpoint rule.
    return {};
  }
  std::vector<std::array<double, 3>> weights(n, {0.0, 0.0, 0.0});
  for (size_t i = 0; i < n; ++i) {
    normal_matrix[i * n + i] += 1e-7 * trace / n;
  }
  for (int c = 0; c < 3; ++c) {
    std::vector<double> w = SolveCholesky(normal_matrix, rhs[c]);
    for (size_t i = 0; i < n; ++i) {
      weights[i][c] = w[i];
    }
  }
  return weights;
}

/*
<h4 id="sun_transmittance">Spectral sun transmittance</h4>

<p>In precomputed illuminance mode, the scattering textures are integrated over
the whole spectrum but, in the OpenGL version, the transmittance texture
contains the transmittance $T(\lambda)$ at the 3 wavelengths
<code>kLambdaR</code>, <code>kLambdaG</code>, <code>kLambdaB</code> only. The
Sun disc, the direct sunlight on surfaces and the aerial perspective are then
computed as this transmittance times linear sRGB values, for instance $T(680)
\int S\bar{r}d\lambda$ for the red channel of the Sun disc, instead of the
correct value $\int ST\bar{r}d\lambda$. This is only correct if $T$ is constant
over each channel, which is far from being the case for long optical paths: the
Rayleigh extinction varies like $\lambda^{-4}$, and the ozone absorption has a
large peak around 600nm. For instance, near sunset, the transmittance at 680nm
is much larger than its average over the red channel (which is mostly sensitive
around 600nm), which gives too red and too bright sunsets. To give an idea of
these errors, we evaluated them with a simple offline single scattering model
(not included here), for a ground observer, Sun zenith angles between 0 and 90
degrees, and horizontal view paths up to 150km, compared with a 1nm spectral
integration: the mean CIEDE2000 chroma error was about 5.6, and the mean
luminance error about 40% (these figures are only indicative).

<p>The <code>spectral_sun_transmittance</code> option fixes this in two parts.
First, the Sun light reaching a point $\mathbf{p}$ at radius $r$, with a Sun
zenith angle cosine $\mu_s$, has been attenuated along the path from the top of
the atmosphere to $\mathbf{p}$, which only depends on $(r,\mu_s)$. Its exact
linear sRGB color is thus
$\int S(\lambda)T(\lambda;r,\mu_s)\bar{c}(\lambda)d\lambda$, which we can
precompute in a texture with the same parameterization as the transmittance
texture. More precisely, we store the "effective" transmittance
$$T_{\mathrm{eff},c}(r,\mu)=\frac{\int S(\lambda)T(\lambda;r,\mu)\bar{c}
(\lambda)d\lambda}{\int S(\lambda)\bar{c}(\lambda)d\lambda}$$
whose denominator is the linear sRGB color of the Sun outside the atmosphere,
i.e. the value returned by <code>GetSolarLuminance</code> (up to a constant
factor): indeed, <code>sun_spectral_radiance_to_luminance</code> is
<code>MAX_LUMINOUS_EFFICACY</code> times $\int S\bar{c}d\lambda$ divided by $S$
at <code>kLambdaR</code>, <code>kLambdaG</code> or <code>kLambdaB</code> (see
<code>ComputeSpectralRadianceToLuminanceFactors</code>), and
<code>GetSolarLuminance</code> multiplies it with $S$ at these wavelengths.
Thus <code>GetSolarLuminance</code> times $T_{\mathrm{eff}}$ gives the exact
color of the attenuated Sun disc, and of the direct sunlight on surfaces (up to
the precision of the numerical integration and of the texture interpolation,
and except for the longest paths, near the horizon, where this exact color is
outside the sRGB gamut - with a negative blue, or even green, component - which
is clamped to 0). Note that, because of this, a low Sun rendered with a large
exposure and a per channel tone mapping can show a sharp white/yellow split
inside the Sun disc, along the contour where the blue component becomes 0: the
small positive blue values above it are saturated by the tone mapping, while
the clamped values below it are not (by default our demo uses a tone mapping
which mixes the color channels, AgX, which avoids this).

<p>This texture can't be used for the transmittance between two arbitrary points
$\mathbf{p}$ and $\mathbf{q}$ (needed for the aerial perspective) though. For a
single wavelength this transmittance is the ratio of the transmittances from
$\mathbf{p}$ and from $\mathbf{q}$ to the top of the atmosphere (see
<code>GetTransmittance</code>), but this is not true for $T_{\mathrm{eff}}$,
since the ratio of two spectral averages is not the average of the ratio (in
the above offline model, using this ratio for the segments gives large chroma
errors, about twice as large as with the 3 wavelengths transmittance).
Moreover, the light transmitted along such segments does not have the
solar spectrum (e.g. it is reflected by the ground, or scattered by the
atmosphere), so a single effective transmittance can't be exact anyway.
Instead, the second part of this option computes the (normal) transmittance
texture with the extinction coefficients $\beta(\lambda)$ of each component
(Rayleigh, Mie and absorption) replaced with their averages over each linear
sRGB channel $c$, weighted by the solar spectrum:
$$\beta_c=\frac{\int S(\lambda)\beta(\lambda)\bar{c}(\lambda)d\lambda}
{\int S(\lambda)\bar{c}(\lambda)d\lambda}$$
This keeps the ratio property of a single wavelength transmittance and, for
short paths (where $T\approx 1-\beta l$), gives the exact color of the
attenuated sunlight, to first order in the optical depth (the point sampled
values are not even exact to first order). With both parts, the errors of the
above offline model drop to about 1.7 (chroma) and 4% (luminance), the
remaining errors coming only from the aerial perspective (the Sun disc and the
direct sunlight being exact).

<p>The band-averaged coefficients are computed as in the demo's "Rec.709
band-averaged coefficients" option, but here only for the extinction
coefficients, and only for the transmittance texture. The products
$S(\lambda)\beta(\lambda)$ are computed at the input wavelengths, and then
integrated with <code>ConvertSpectrumToLinearSrgb</code>. If the solar color is
not positive in some channel (e.g. with a null solar spectrum), we fall back to
the point sampled value in this channel:
*/

// Returns the averages of 'spectrum' over the 3 linear sRGB channels, weighted
// by 'solar_irradiance' (and clamped to non-negative values, since the signed
// color matching functions can give slightly negative values, e.g. for the
// ozone absorption in the blue channel), times 'scale'.
hlsl::float3 ComputeBandAverage(const std::vector<double>& wavelengths,
                                const std::vector<double>& solar_irradiance,
                                const std::vector<double>& spectrum,
                                double scale) {
  std::vector<double> weighted(wavelengths.size());
  for (size_t i = 0; i < wavelengths.size(); ++i) {
    weighted[i] = solar_irradiance[i] * spectrum[i];
  }
  double solar[3];
  double weighted_rgb[3];
  Model::ConvertSpectrumToLinearSrgb(wavelengths, solar_irradiance,
                                     &solar[0], &solar[1], &solar[2]);
  Model::ConvertSpectrumToLinearSrgb(wavelengths, weighted, &weighted_rgb[0],
                                     &weighted_rgb[1], &weighted_rgb[2]);
  const double lambdas[3] = {Model::kLambdaR, Model::kLambdaG,
                             Model::kLambdaB};
  hlsl::float3 result;
  for (int c = 0; c < 3; ++c) {
    double value = solar[c] > 0.0
        ? std::max(weighted_rgb[c] / solar[c], 0.0)
        : Interpolate(wavelengths, spectrum, lambdas[c]);
    result[c] = static_cast<float>(value * scale);
  }
  return result;
}

/*
<p>The effective transmittance texture is computed with a numerical
integration over the wavelength, with the same 1nm step as in
<code>ConvertSpectrumToLinearSrgb</code> (i.e. 470 samples between 360 and
829nm), so that its denominator is exactly the solar color returned by
<code>GetSolarLuminance</code>. Since the 3 optical lengths to the top of the
atmosphere (one per density profile) don't depend on the wavelength, each texel
only needs these 3 integrals (as for the normal transmittance texture), plus
470 exponentials. The shader (in <a href=
"../../shaders/atmosphere/precompute.hlsl.html">precompute.hlsl</a>) gets the
extinction coefficients $\beta_R,\beta_M^{ext},\beta_O^{ext}$ (in inverse
length units) and the normalized weights $S\bar{c}/\int S\bar{c}d\lambda$ at
each wavelength from the following constant buffer, which we fill in the
constructor (the input spectra are linearly interpolated at each wavelength;
if the solar color is not positive in some channel, all the weight is put on
the sample closest to <code>kLambdaR</code>, <code>kLambdaG</code> or
<code>kLambdaB</code>, which then gives the point sampled transmittance in this
channel):
*/

constexpr int kSunTransmittanceSpectrumSize = kLambdaMax - kLambdaMin;

struct SunTransmittanceSpectrum {
  // Rayleigh, Mie and absorption extinction coefficients (and padding).
  float extinction[kSunTransmittanceSpectrumSize][4];
  // Normalized weights of the R, G, B channels (and padding).
  float weights[kSunTransmittanceSpectrumSize][4];
};
static_assert(sizeof(SunTransmittanceSpectrum) ==
              2 * kSunTransmittanceSpectrumSize * 4 * sizeof(float));

std::vector<std::array<float, 4>> ComputeSunTransmittanceSpectrum(
    const std::vector<double>& wavelengths,
    const std::vector<double>& solar_irradiance,
    const std::vector<double>& rayleigh_scattering,
    const std::vector<double>& mie_extinction,
    const std::vector<double>& absorption_extinction,
    double length_unit_in_meters) {
  constexpr int kSize = kSunTransmittanceSpectrumSize;
  std::vector<std::array<float, 4>> result(2 * kSize);
  std::vector<std::array<double, 3>> weights(kSize);
  double weight_sums[3] = {0.0, 0.0, 0.0};
  for (int j = 0; j < kSize; ++j) {
    const double lambda = kLambdaMin + j;
    result[j] = {
        static_cast<float>(Interpolate(wavelengths, rayleigh_scattering,
                                       lambda) * length_unit_in_meters),
        static_cast<float>(Interpolate(wavelengths, mie_extinction, lambda) *
                           length_unit_in_meters),
        static_cast<float>(Interpolate(wavelengths, absorption_extinction,
                                       lambda) * length_unit_in_meters),
        0.0f};
    const double solar = Interpolate(wavelengths, solar_irradiance, lambda);
    for (int c = 0; c < 3; ++c) {
      weights[j][c] = SrgbColorMatchingFunction(lambda, c) * solar;
      weight_sums[c] += weights[j][c];
    }
  }
  const double lambdas[3] = {Model::kLambdaR, Model::kLambdaG,
                             Model::kLambdaB};
  for (int c = 0; c < 3; ++c) {
    const int fallback_index = std::clamp(
        static_cast<int>(std::lround(lambdas[c])) - kLambdaMin, 0, kSize - 1);
    for (int j = 0; j < kSize; ++j) {
      double weight = weight_sums[c] > 0.0 ? weights[j][c] / weight_sums[c]
                                           : (j == fallback_index ? 1.0 : 0.0);
      result[kSize + j][c] = static_cast<float>(weight);
    }
  }
  return result;
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
    bool half_precision,
    bool accumulate_in_float32,
    WavelengthRule wavelength_rule,
    bool spectral_sun_transmittance) :
        device_(device),
        descriptor_heap_(descriptor_heap),
        shader_compiler_(shader_compiler),
        num_precomputed_wavelengths_(num_precomputed_wavelengths),
        combine_scattering_textures_(combine_scattering_textures),
        half_precision_(half_precision),
        accumulate_in_float32_(accumulate_in_float32),
        spectral_sun_transmittance_(
            spectral_sun_transmittance && num_precomputed_wavelengths > 3) {
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

/*
<p>With the <code>spectral_sun_transmittance</code> option, the constructor
also computes the atmosphere parameters used for the final transmittance
texture, and the spectral tables used for the sun transmittance texture (see
the <a href="#sun_transmittance">utility functions</a>). Note that only the
transmittance texture uses the band-averaged extinction coefficients. The
atmosphere parameters returned by <code>atmosphere_parameters()</code>, used by
the rendering shaders, are unchanged. Indeed, these shaders only read the solar
irradiance (for <code>GetSolarLuminance</code>, already correct as explained
above), the Sun angular radius, the radii and <code>mu_s_min</code> (for the
texture coordinates), the Mie phase function parameter, and the Rayleigh and
Mie scattering coefficients (only in the ratios used to extrapolate the single
Mie scattering from the combined scattering texture). None of them is an
extinction coefficient, and changing them would change the sky in-scattering,
which is already integrated over the whole spectrum:
*/

  if (spectral_sun_transmittance_) {
    transmittance_parameters_ = atmosphere_parameters_;
    transmittance_parameters_.rayleigh_scattering = ComputeBandAverage(
        wavelengths, solar_irradiance, rayleigh_scattering,
        length_unit_in_meters);
    transmittance_parameters_.mie_extinction = ComputeBandAverage(
        wavelengths, solar_irradiance, mie_extinction, length_unit_in_meters);
    transmittance_parameters_.absorption_extinction = ComputeBandAverage(
        wavelengths, solar_irradiance, absorption_extinction,
        length_unit_in_meters);
    sun_transmittance_spectrum_ = ComputeSunTransmittanceSpectrum(
        wavelengths, solar_irradiance, rayleigh_scattering, mie_extinction,
        absorption_extinction, length_unit_in_meters);
  }

/*
<p>In precomputed illuminance mode, the constructor also computes the
wavelengths of each precomputation pass, and the corresponding
<code>luminance_from_radiance</code> matrices, from the wavelength integration
rule (see the <a href="#utilities">utility functions</a>). With a fitted rule,
the matrix coefficients are the weights fitted for the input spectra. With the
midpoint rule, as in the OpenGL version (and as a fallback if the fit fails),
the number of wavelengths is rounded up to a multiple of 3, and the matrix
coefficients are the sRGB color matching functions times the wavelength
spacing:
*/

  if (precompute_illuminance && wavelength_rule != WavelengthRule::kMidpoint) {
    const std::vector<double> nodes =
        wavelength_rule == WavelengthRule::kFitted6
            ? std::vector<double>(std::begin(kFitted6Wavelengths),
                                  std::end(kFitted6Wavelengths))
            : std::vector<double>(std::begin(kFitted9Wavelengths),
                                  std::end(kFitted9Wavelengths));
    const std::vector<std::array<double, 3>> weights =
        FitWavelengthRuleWeights(nodes, wavelengths, solar_irradiance,
            rayleigh_scattering, mie_extinction, absorption_extinction,
            ComputeVerticalColumn(rayleigh_density, bottom_radius,
                                  top_radius),
            ComputeVerticalColumn(mie_density, bottom_radius, top_radius),
            ComputeVerticalColumn(absorption_density, bottom_radius,
                                  top_radius));
    // Each pass uses 3 consecutive nodes (the number of nodes of the fitted
    // rules is a multiple of 3). If the fit failed (empty weights), the
    // midpoint rule is used instead (see below).
    for (size_t i = 0; i + 2 < weights.size(); i += 3) {
      WavelengthPass pass;
      for (int column = 0; column < 3; ++column) {
        pass.lambdas[column] = nodes[i + column];
        for (int row = 0; row < 3; ++row) {
          pass.luminance_from_radiance[3 * row + column] =
              static_cast<float>(weights[i + column][row]);
        }
      }
      wavelength_passes_.push_back(pass);
    }
  }
  if (precompute_illuminance && wavelength_passes_.empty()) {
    const unsigned int num_iterations = (num_precomputed_wavelengths + 2) / 3;
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
        // sky_spectral_radiance_to_luminance (see above).
        return static_cast<float>(
            SrgbColorMatchingFunction(lambda, component) * dlambda);
      };
      mat3 luminance_from_radiance{
        coeff(lambdas[0], 0), coeff(lambdas[1], 0), coeff(lambdas[2], 0),
        coeff(lambdas[0], 1), coeff(lambdas[1], 1), coeff(lambdas[2], 1),
        coeff(lambdas[0], 2), coeff(lambdas[1], 2), coeff(lambdas[2], 2)
      };
      wavelength_passes_.push_back({lambdas, luminance_from_radiance});
    }
  }

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
  if (spectral_sun_transmittance_) {
    sun_transmittance_texture_ = d3d12::Texture::Create2d(
        d3d_device, descriptor_heap_, TRANSMITTANCE_TEXTURE_WIDTH,
        TRANSMITTANCE_TEXTURE_HEIGHT, kTexture2dFormat, true,
        L"Sun transmittance texture");
  }
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
  if (spectral_sun_transmittance_) {
    defines.push_back({"SPECTRAL_SUN_TRANSMITTANCE", ""});
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
  // Only used with the accumulate_in_float32 option (see below).
  d3d12::ComPtr<ID3D12PipelineState> convert_to_half;
  // Only used with the spectral_sun_transmittance option. This shader uses an
  // additional constant buffer, and thus its own root signature.
  d3d12::ComPtr<ID3D12RootSignature> sun_transmittance_root_signature;
  d3d12::ComPtr<ID3D12PipelineState> compute_sun_transmittance;
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
  // With the accumulate_in_float32 option, the scattering orders (and the
  // groups of 3 wavelengths) are accumulated in the following single precision
  // textures (see Init), instead of directly in the final scattering_texture_
  // and optional_single_mie_scattering_texture_.
  bool accumulate_in_float32 = false;
  d3d12::Texture float32_scattering;
  d3d12::Texture float32_single_mie_scattering;
  // The textures where the scattering orders are accumulated.
  d3d12::Texture& scattering(Model& model) {
    return accumulate_in_float32 ? float32_scattering
                                 : model.scattering_texture_;
  }
  d3d12::Texture& single_mie_scattering(Model& model) {
    return accumulate_in_float32
        ? float32_single_mie_scattering
        : model.optional_single_mie_scattering_texture_;
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
the wavelengths of each pass computed in the constructor, 3 at a time, each
time converting the results to luminance (with the
<code>luminance_from_radiance</code> matrix of this pass, whose coefficients
can be negative with the signed sRGB color matching functions, and with the
fitted rules), and accumulating them in the
final textures (via the <code>blend</code> argument) or, with the
<code>accumulate_in_float32</code> option, in temporary single precision
textures converted to the final half precision textures at the end (see
below).</li>
</ul>
Since the atmosphere parameters and the luminance_from_radiance matrix are
stored in a constant buffer, instead of being compiled in the shaders, each
group of 3 wavelengths simply uses a different part of an upload buffer:
*/

void Model::Init(unsigned int num_scattering_orders) {
  ID3D12Device* d3d_device = device_->device();
  // The accumulate_in_float32 option is useless if the final textures already
  // use single precision floats.
  const bool accumulate_in_float32 = accumulate_in_float32_ && half_precision_;

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
  d3d12::Shader convert_to_half_shader;
  if (accumulate_in_float32) {
    convert_to_half_shader = compile("ConvertToHalfCS");
  }
  d3d12::Shader sun_transmittance_shader;
  if (spectral_sun_transmittance_) {
    // The size of the spectral tables is passed with a macro, which must be
    // defined to compile this entry point (see precompute.hlsl).
    std::vector<d3d12::ShaderDefine> sun_defines = defines;
    sun_defines.push_back({"SUN_TRANSMITTANCE_SPECTRUM_SIZE",
                           std::to_string(kSunTransmittanceSpectrumSize)});
    sun_transmittance_shader = shader_compiler_->Compile(
        "atmosphere/precompute.hlsl", "ComputeSunTransmittanceCS", "cs_6_6",
        sun_defines);
  }
#ifndef NDEBUG
  // Check that the C++ and HLSL constant buffer layouts are the same.
  auto members = hlsl::GetAtmosphereParametersLayout("constants.atmosphere", 0);
  members.push_back({"constants.luminance_from_radiance",
                     offsetof(PrecomputeConstants, luminance_from_radiance)});
  d3d12::ValidateConstantBufferLayout(shaders[2], "constants", members,
                                      sizeof(PrecomputeConstants));
  if (spectral_sun_transmittance_) {
    const d3d12::ConstantBufferMember spectrum_members[] = {
        {"sun_transmittance_spectrum.extinction",
         offsetof(SunTransmittanceSpectrum, extinction)},
        {"sun_transmittance_spectrum.weights",
         offsetof(SunTransmittanceSpectrum, weights)}};
    d3d12::ValidateConstantBufferLayout(sun_transmittance_shader,
        "sun_transmittance_spectrum", spectrum_members,
        sizeof(SunTransmittanceSpectrum));
  }
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
  auto create_pipeline_state = [&](const d3d12::Shader& shader,
                                   d3d12::ComPtr<ID3D12PipelineState>* state) {
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = programs.root_signature.Get();
    desc.CS = shader.GetBytecode();
    D3D12_CHECK(d3d_device->CreateComputePipelineState(
        &desc, IID_PPV_ARGS(state->GetAddressOf())));
  };
  for (int i = 0; i < 6; ++i) {
    create_pipeline_state(shaders[i], pipeline_states[i]);
  }
  if (accumulate_in_float32) {
    create_pipeline_state(convert_to_half_shader, &programs.convert_to_half);
  }
  if (spectral_sun_transmittance_) {
    programs.sun_transmittance_root_signature =
        d3d12::CreateRootSignature(d3d_device, sun_transmittance_shader);
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = programs.sun_transmittance_root_signature.Get();
    desc.CS = sun_transmittance_shader.GetBytecode();
    D3D12_CHECK(d3d_device->CreateComputePipelineState(
        &desc, IID_PPV_ARGS(&programs.compute_sun_transmittance)));
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

/*
<p>The final scattering textures are the sum of the contributions of each
scattering order and, in precomputed illuminance mode, of each group of 3
wavelengths. With half precision textures, accumulating these contributions
directly in the final textures rounds each partial sum to half precision. The
rounding errors thus add up with the number of terms and, with many
wavelengths, they give noticeable color errors. To avoid this, the
<code>accumulate_in_float32</code> option accumulates these contributions in
temporary single precision textures instead, converted only once to half
precision at the end of the precomputations (the delta textures, which store a
single contribution, still use the precision of the final textures). Otherwise,
the contributions are directly accumulated in the final textures:
*/

  textures.accumulate_in_float32 = accumulate_in_float32;
  if (accumulate_in_float32) {
    textures.float32_scattering = d3d12::Texture::Create3d(
        d3d_device, descriptor_heap_, SCATTERING_TEXTURE_WIDTH,
        SCATTERING_TEXTURE_HEIGHT, SCATTERING_TEXTURE_DEPTH,
        DXGI_FORMAT_R32G32B32A32_FLOAT, true, L"Float32 scattering texture");
    if (!combine_scattering_textures_) {
      textures.float32_single_mie_scattering = d3d12::Texture::Create3d(
          d3d_device, descriptor_heap_, SCATTERING_TEXTURE_WIDTH,
          SCATTERING_TEXTURE_HEIGHT, SCATTERING_TEXTURE_DEPTH,
          DXGI_FORMAT_R32G32B32A32_FLOAT, true,
          L"Float32 single Mie scattering texture");
    }
  }

  // Allocate an upload buffer for the constants of each group of 3 wavelengths
  // (plus one for the final transmittance pass in precomputed illuminance
  // mode, and one for the spectral tables of the spectral_sun_transmittance
  // option), as well as a command list and a GPU timer.
  const unsigned int num_iterations = wavelength_passes_.empty()
      ? 1 : static_cast<unsigned int>(wavelength_passes_.size());
  const size_t spectrum_offset = kConstantsSize * (num_iterations + 1);
  const size_t spectrum_size = spectral_sun_transmittance_
      ? (sizeof(SunTransmittanceSpectrum) + kConstantBufferAlignment - 1) &
            ~(kConstantBufferAlignment - 1)
      : 0;
  d3d12::ComPtr<ID3D12Resource> constants_buffer;
  CD3DX12_HEAP_PROPERTIES upload_heap(D3D12_HEAP_TYPE_UPLOAD);
  auto buffer_desc =
      CD3DX12_RESOURCE_DESC::Buffer(spectrum_offset + spectrum_size);
  D3D12_CHECK(d3d_device->CreateCommittedResource(
      &upload_heap, D3D12_HEAP_FLAG_NONE, &buffer_desc,
      D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
      IID_PPV_ARGS(&constants_buffer)));
  uint8_t* constants_data;
  D3D12_CHECK(constants_buffer->Map(
      0, nullptr, reinterpret_cast<void**>(&constants_data)));
  auto set_constants = [&](unsigned int index,
                           const hlsl::AtmosphereParameters& atmosphere,
                           const mat3& luminance_from_radiance) {
    PrecomputeConstants constants = {};
    constants.atmosphere = atmosphere;
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
      num_iterations * 3 * num_scattering_orders + 1 +
          (accumulate_in_float32 ? 1 : 0) +
          (spectral_sun_transmittance_ ? 1 : 0));

  // The descriptor heap must be set before the root signature, since the root
  // signature uses the CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED flag.
  ID3D12DescriptorHeap* heaps[] = {descriptor_heap_->heap()};
  command_list->SetDescriptorHeaps(1, heaps);
  command_list->SetComputeRootSignature(programs.root_signature.Get());

  if (wavelength_passes_.empty()) {
    vec3 lambdas{kLambdaR, kLambdaG, kLambdaB};
    mat3 luminance_from_radiance{1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
    Precompute(command_list.Get(), &timer, programs, textures,
        set_constants(0, atmosphere_parameters_factory_(lambdas),
                      luminance_from_radiance),
        false /* blend */, num_scattering_orders);
  } else {
    for (unsigned int i = 0; i < num_iterations; ++i) {
      const WavelengthPass& pass = wavelength_passes_[i];
      Precompute(command_list.Get(), &timer, programs, textures,
          set_constants(i, atmosphere_parameters_factory_(pass.lambdas),
                        pass.luminance_from_radiance),
          i > 0 /* blend */, num_scattering_orders);
    }

    // After the above iterations, the transmittance texture contains the
    // transmittance for the 3 wavelengths used at the last iteration. But we
    // want the transmittance at kLambdaR, kLambdaG, kLambdaB instead (or, with
    // the spectral_sun_transmittance option, with the band-averaged extinction
    // coefficients), so we must recompute it here:
    mat3 identity{1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
    command_list->SetComputeRootConstantBufferView(0, set_constants(
        num_iterations, spectral_sun_transmittance_
            ? transmittance_parameters_
            : atmosphere_parameters_factory_({kLambdaR, kLambdaG, kLambdaB}),
        identity));
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

  // With the accumulate_in_float32 option, convert the accumulated results to
  // the final half precision textures, with one dispatch per texture (i.e. one
  // dispatch, or two with a separate single Mie scattering texture).
  if (accumulate_in_float32) {
    constexpr D3D12_RESOURCE_STATES kSrv = d3d12::Texture::kShaderResourceState;
    constexpr D3D12_RESOURCE_STATES kUav =
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    d3d12::Transition(command_list.Get(), {
        {&textures.float32_scattering, kSrv},
        {&scattering_texture_, kUav}});
    if (!combine_scattering_textures_) {
      d3d12::Transition(command_list.Get(), {
          {&textures.float32_single_mie_scattering, kSrv},
          {&optional_single_mie_scattering_texture_, kUav}});
    }
    auto convert = [&](const d3d12::Texture& input,
                       const d3d12::Texture& output) {
      PrecomputeResources resources = {};
      resources.multiple_scattering_texture = input.srv();
      resources.output0 = output.uav();
      command_list->SetComputeRoot32BitConstants(
          1, sizeof(resources) / 4, &resources, 0);
      command_list->Dispatch(GroupCount(SCATTERING_TEXTURE_WIDTH),
                             GroupCount(SCATTERING_TEXTURE_HEIGHT),
                             SCATTERING_TEXTURE_DEPTH);
    };
    timer.Begin(command_list.Get(), "Convert to half");
    command_list->SetPipelineState(programs.convert_to_half.Get());
    convert(textures.float32_scattering, scattering_texture_);
    if (!combine_scattering_textures_) {
      convert(textures.float32_single_mie_scattering,
              optional_single_mie_scattering_texture_);
    }
    timer.End(command_list.Get());
  }

/*
<p>At the end, with the <code>spectral_sun_transmittance</code> option, we
compute the sun transmittance texture (see the <a href="#sun_transmittance"
>utility functions</a>), with a single dispatch. Its shader uses the same
constant buffer as the final transmittance pass (for the radii and the density
profiles), plus the spectral tables in an additional constant buffer, and thus
its own root signature. Setting this root signature resets the root arguments,
which must therefore be set again (we do this last, so that the other dispatches
are not affected):
*/

  if (spectral_sun_transmittance_) {
    memcpy(constants_data + spectrum_offset,
           sun_transmittance_spectrum_.data(),
           sizeof(SunTransmittanceSpectrum));
    command_list->SetComputeRootSignature(
        programs.sun_transmittance_root_signature.Get());
    command_list->SetComputeRootConstantBufferView(0,
        constants_buffer->GetGPUVirtualAddress() +
            num_iterations * kConstantsSize);
    command_list->SetComputeRootConstantBufferView(2,
        constants_buffer->GetGPUVirtualAddress() + spectrum_offset);
    d3d12::Transition(command_list.Get(), {
        {&sun_transmittance_texture_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS}});
    PrecomputeResources resources = {};
    resources.output0 = sun_transmittance_texture_.uav();
    command_list->SetComputeRoot32BitConstants(
        1, sizeof(resources) / 4, &resources, 0);
    timer.Begin(command_list.Get(), "Sun transmittance");
    command_list->SetPipelineState(programs.compute_sun_transmittance.Get());
    command_list->Dispatch(GroupCount(TRANSMITTANCE_TEXTURE_WIDTH),
                           GroupCount(TRANSMITTANCE_TEXTURE_HEIGHT), 1);
    timer.End(command_list.Get());
    d3d12::Transition(command_list.Get(), {
        {&sun_transmittance_texture_, d3d12::Texture::kShaderResourceState}});
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
  // either store them or accumulate them in textures.scattering() and
  // textures.single_mie_scattering() (i.e. in scattering_texture_ and
  // optional_single_mie_scattering_texture_, or in their single precision
  // temporary versions, see Init).
  d3d12::Texture& scattering = textures.scattering(*this);
  d3d12::Transition(command_list, {
      {&textures.delta_rayleigh_scattering, kUav},
      {&textures.delta_mie_scattering, kUav},
      {&scattering, kUav}});
  resources = {};
  resources.transmittance_texture = transmittance_texture_.srv();
  resources.output0 = textures.delta_rayleigh_scattering.uav();
  resources.output1 = textures.delta_mie_scattering.uav();
  resources.output2 = scattering.uav();
  if (!combine_scattering_textures_) {
    d3d12::Texture& single_mie_scattering =
        textures.single_mie_scattering(*this);
    d3d12::Transition(command_list, {{&single_mie_scattering, kUav}});
    resources.output3 = single_mie_scattering.uav();
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
    // textures.scattering().
    d3d12::Transition(command_list, {
        {&textures.delta_scattering_density, kSrv},
        {&textures.delta_multiple_scattering(), kUav}});
    resources = {};
    resources.transmittance_texture = transmittance_texture_.srv();
    resources.scattering_density_texture =
        textures.delta_scattering_density.srv();
    resources.output0 = textures.delta_multiple_scattering().uav();
    resources.output1 = scattering.uav();
    resources.blend = 0b10;
    dispatch("Multiple scattering", programs.compute_multiple_scattering.Get(),
             resources, SCATTERING_TEXTURE_WIDTH, SCATTERING_TEXTURE_HEIGHT,
             SCATTERING_TEXTURE_DEPTH);
  }
}

}  // namespace atmosphere
