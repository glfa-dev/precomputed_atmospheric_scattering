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

/*<h2>src/demo/demo.h</h2>

<p>Our demo application is implemented as a single <code>Demo</code> class,
whose header is defined below. It is a port of the OpenGL demo
(<code>atmosphere/demo/demo.h</code>), with a Dear ImGui user interface instead
of keyboard shortcuts (which are still supported), and with all the atmosphere
parameters editable at runtime. The window, the swap chain and the frame loop
are managed separately, in <a href="main.cc.html">main.cc</a>.
*/

#ifndef DEMO_DEMO_H_
#define DEMO_DEMO_H_

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "atmosphere/model.h"
#include "d3d12/common.h"
#include "d3d12/descriptor_heap.h"
#include "d3d12/device.h"
#include "d3d12/shader_compiler.h"
#include "d3d12/shader_watcher.h"
#include "d3d12/texture.h"

namespace atmosphere {
namespace demo {

/*
<p>The atmosphere parameters, with default values corresponding to the Earth
atmosphere (the same as in the OpenGL demo). The wavelength dependent
parameters are computed from these values in <code>Demo::InitModel</code>:
*/

struct AtmosphereSettings {
  bool use_constant_solar_spectrum = false;
  // Wavelength independent solar irradiance "spectrum" (not physically
  // realistic, but was used in the original implementation), in W/m^2/nm.
  double constant_solar_irradiance = 1.5;
  // Whether to replace the wavelength dependent parameters with their averages
  // over the 3 linear sRGB (Rec.709) channels, weighted by the solar spectrum
  // and the sRGB color matching functions, instead of sampling them at
  // kLambdaR, kLambdaG, kLambdaB (see Demo::InitModel). Only compatible with
  // the radiance (i.e. non luminance) rendering mode.
  bool use_rgb_coefficients = false;
  double sun_angular_radius = 0.00935 / 2.0;
  double bottom_radius = 6360000.0;
  double top_radius = 6420000.0;
  // The Rayleigh scattering coefficient is equal to this value (in m^-1) times
  // lambda^-4, with lambda in micrometers.
  double rayleigh = 1.24062e-6;
  std::array<DensityProfileLayer, 2> rayleigh_density = {
      DensityProfileLayer(),
      DensityProfileLayer(0.0, 1.0, -1.0 / 8000.0, 0.0, 0.0)};
  // The Mie extinction coefficient is equal to 'mie_extinction' (in m^-1)
  // times lambda^-mie_angstrom_alpha, with lambda in micrometers.
  double mie_extinction = 5.328e-3 / 1200.0;
  double mie_angstrom_alpha = 0.0;
  double mie_single_scattering_albedo = 0.9;
  double mie_phase_function_g = 0.8;
  std::array<DensityProfileLayer, 2> mie_density = {
      DensityProfileLayer(),
      DensityProfileLayer(0.0, 1.0, -1.0 / 1200.0, 0.0, 0.0)};
  bool use_ozone = true;
  // The total amount of ozone, in Dobson units.
  double ozone_dobson_units = 300.0;
  // Density profile increasing linearly from 0 to 1 between 10 and 25km, and
  // decreasing linearly from 1 to 0 between 25 and 40km.
  std::array<DensityProfileLayer, 2> absorption_density = {
      DensityProfileLayer(25000.0, 0.0, 0.0, 1.0 / 15000.0, -2.0 / 3.0),
      DensityProfileLayer(0.0, 0.0, 0.0, -1.0 / 15000.0, 8.0 / 3.0)};
  double ground_albedo = 0.1;
  // 102 degrees with half precision textures, 120 degrees otherwise.
  double max_sun_zenith_angle_degrees = 102.0;
  bool use_combined_textures = true;
  bool use_half_precision = true;
  // Whether to accumulate the precomputations in single precision textures,
  // converted to half precision at the end (only with half precision).
  bool accumulate_in_float32 = false;
  // The number of wavelengths in precomputed luminance mode (with the
  // midpoint rule).
  int num_precomputed_wavelengths = 15;
  // The wavelength integration rule in precomputed luminance mode (the
  // fitted rules use a fixed number of wavelengths, see Model).
  WavelengthRule wavelength_rule = WavelengthRule::kMidpoint;
  int num_scattering_orders = 4;
};

class Demo {
 public:
  Demo(d3d12::Device* device, d3d12::DescriptorHeap* descriptor_heap,
       const std::filesystem::path& shader_directory,
       uint32_t num_frames_in_flight);
  ~Demo();

  // Must be called at the beginning of each frame, before recording any
  // command. Reloads the shaders if their source files changed, and
  // re-initializes the atmosphere model if its parameters changed (this
  // waits for the GPU to be idle).
  void Update();
  // Records the commands to render the scene in the given render target.
  void Render(ID3D12GraphicsCommandList* command_list, uint32_t frame_index,
              D3D12_CPU_DESCRIPTOR_HANDLE render_target, uint32_t width,
              uint32_t height);
  // Builds the Dear ImGui windows (between ImGui::NewFrame and ImGui::Render).
  void DrawUi();

  void HandleKey(char key);
  void HandleMouseDrag(int delta_x, int delta_y, bool move_sun);
  void HandleMouseWheel(int direction);

 private:
  enum Luminance {
    // Render the spectral radiance at kLambdaR, kLambdaG, kLambdaB.
    NONE,
    // Render the sRGB luminance, using an approximate (on the fly) conversion
    // from 3 spectral radiance values only (see section 14.3 in <a href=
    // "https://arxiv.org/pdf/1612.04336.pdf">A Qualitative and Quantitative
    //  Evaluation of 8 Clear Sky Models</a>).
    APPROXIMATE,
    // Render the sRGB luminance, precomputed from 15 spectral radiance values
    // (see section 4.4 in <a href=
    // "http://www.oskee.wz.cz/stranka/uploads/SCCG10ElekKmoch.pdf">Real-time
    //  Spectral Scattering in Large-scale Natural Participating Media</a>).
    PRECOMPUTED
  };

  void InitModel();
  void UpdateWhitePoint();
  void BuildPipelines();
  void ReloadShaders();
  void RenderLutView(ID3D12GraphicsCommandList* command_list);
  void SetView(double view_distance_meters, double view_zenith_angle_radians,
      double view_azimuth_angle_radians, double sun_zenith_angle_radians,
      double sun_azimuth_angle_radians, double exposure);

  // UI panels, implemented in ui.cc.
  void DrawDemoWindow();
  void DrawAtmosphereWindow();
  void DrawLutViewerWindow();
  void DrawTimingsWindow();
  void DrawErrorWindow();

  d3d12::Device* device_;
  d3d12::DescriptorHeap* descriptor_heap_;
  d3d12::ShaderCompiler shader_compiler_;
  d3d12::ShaderWatcher shader_watcher_;
  uint32_t num_frames_in_flight_;

  AtmosphereSettings settings_;
  Luminance use_luminance_;
  bool do_white_balance_;
  bool show_ui_;
  // Whether the model must be re-created (because settings_ changed), and
  // whether the Atmosphere window has unapplied changes.
  bool model_dirty_;
  bool settings_modified_;
  std::string error_message_;
  // The CPU time of the last model initialization, in milliseconds.
  double init_time_milliseconds_;

  std::unique_ptr<Model> model_;
  std::vector<double> wavelengths_;
  std::vector<double> solar_irradiance_;
  float white_point_[3];
  // Whether the current model uses RGB band-averaged coefficients, and their
  // values (solar irradiance, Rayleigh scattering, Mie scattering, Mie
  // extinction, absorption extinction and ground albedo, in R,G,B order).
  bool model_uses_rgb_coefficients_;
  std::array<std::array<double, 3>, 6> rgb_coefficients_;

  d3d12::ComPtr<ID3D12RootSignature> root_signature_;
  d3d12::ComPtr<ID3D12PipelineState> pipeline_state_;
  d3d12::ComPtr<ID3D12Resource> constants_buffer_;
  uint8_t* constants_data_;

  double view_distance_meters_;
  double view_zenith_angle_radians_;
  double view_azimuth_angle_radians_;
  double sun_zenith_angle_radians_;
  double sun_azimuth_angle_radians_;
  double exposure_;

  // The LUT viewer state.
  bool show_lut_viewer_;
  int lut_index_;
  int lut_slice_;
  float lut_scale_;
  bool lut_show_alpha_;
  d3d12::Texture lut_view_texture_;
  d3d12::ComPtr<ID3D12RootSignature> lut_view_root_signature_;
  d3d12::ComPtr<ID3D12PipelineState> lut_view_pipeline_state_;
};

}  // namespace demo
}  // namespace atmosphere

#endif  // DEMO_DEMO_H_
