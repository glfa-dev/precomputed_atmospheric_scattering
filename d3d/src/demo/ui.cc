/*<h2>src/demo/ui.cc</h2>

<p>This file implements the Dear ImGui user interface of the <code>Demo</code>
class. It is independent of the atmosphere model, except for the display of the
precomputed textures and of the precomputation timings. It provides:
<ul>
<li>a "Demo" window, with the options of the OpenGL demo (also available with
the same keyboard shortcuts), the view and Sun angles, and the predefined
views,</li>
<li>an "Atmosphere" window, to edit all the atmosphere model parameters (the
model is re-initialized when the changes are applied),</li>
<li>a "LUT viewer" window, to display the precomputed textures,</li>
<li>a "Precomputation" window, with the GPU time of each precomputation pass,
</li>
<li>an error window, displaying the last shader compilation error.</li>
</ul>
*/

#include <imgui.h>

#include <cmath>

#include "atmosphere/constants.h"
#include "demo/demo.h"

namespace atmosphere {
namespace demo {

namespace {

constexpr double kRadiansToDegrees = 180.0 / 3.14159265358979323846;

// Displays a slider for an angle stored in radians, but displayed in degrees.
bool SliderAngle(const char* label, double* radians, double min_degrees,
                 double max_degrees) {
  double degrees = *radians * kRadiansToDegrees;
  if (ImGui::SliderScalar(label, ImGuiDataType_Double, &degrees, &min_degrees,
                          &max_degrees, "%.2f deg")) {
    *radians = degrees / kRadiansToDegrees;
    return true;
  }
  return false;
}

bool InputDouble(const char* label, double* value, const char* format) {
  return ImGui::InputDouble(label, value, 0.0, 0.0, format);
}

// Displays the fields of a density profile layer, with altitudes in km.
bool EditDensityProfileLayer(const char* label, DensityProfileLayer* layer) {
  bool changed = false;
  ImGui::PushID(label);
  ImGui::TextUnformatted(label);
  ImGui::Indent();
  double width_km = layer->width / 1000.0;
  if (InputDouble("width (km)", &width_km, "%.3f")) {
    layer->width = width_km * 1000.0;
    changed = true;
  }
  changed |= InputDouble("exp_term", &layer->exp_term, "%.6g");
  changed |= InputDouble("exp_scale (1/m)", &layer->exp_scale, "%.6g");
  changed |= InputDouble("linear_term (1/m)", &layer->linear_term, "%.6g");
  changed |= InputDouble("constant_term", &layer->constant_term, "%.6g");
  ImGui::Unindent();
  ImGui::PopID();
  return changed;
}

bool EditDensityProfile(const char* label,
                        std::array<DensityProfileLayer, 2>* layers) {
  bool changed = false;
  if (ImGui::TreeNode(label)) {
    ImGui::TextWrapped("density = exp_term * exp(exp_scale * h) + "
                       "linear_term * h + constant_term, clamped to [0,1]. "
                       "The bottom layer is used below its width, the top "
                       "layer above.");
    changed |= EditDensityProfileLayer("Bottom layer", &(*layers)[0]);
    changed |= EditDensityProfileLayer("Top layer", &(*layers)[1]);
    ImGui::TreePop();
  }
  return changed;
}

}  // anonymous namespace

void Demo::DrawUi() {
  if (!show_ui_) {
    return;
  }
  DrawDemoWindow();
  DrawAtmosphereWindow();
  if (show_lut_viewer_) {
    DrawLutViewerWindow();
  }
  DrawTimingsWindow();
  DrawErrorWindow();
}

void Demo::DrawDemoWindow() {
  ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(360, 0), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Demo")) {
    ImGui::End();
    return;
  }
  ImGui::TextDisabled("%s", d3d12::ToUtf8(device_->adapter_name()).c_str());
  ImGui::TextDisabled("%.1f fps", ImGui::GetIO().Framerate);

  if (ImGui::CollapsingHeader("Model options",
                              ImGuiTreeNodeFlags_DefaultOpen)) {
    bool changed = false;
    changed |= ImGui::Checkbox("Constant solar spectrum (s)",
                               &settings_.use_constant_solar_spectrum);
    changed |= ImGui::Checkbox("Ozone (o)", &settings_.use_ozone);
    changed |= ImGui::Checkbox("Combined textures (t)",
                               &settings_.use_combined_textures);
    if (ImGui::Checkbox("Half precision (p)", &settings_.use_half_precision)) {
      settings_.max_sun_zenith_angle_degrees =
          settings_.use_half_precision ? 102.0 : 120.0;
      changed = true;
    }
    ImGui::BeginDisabled(!settings_.use_half_precision);
    changed |= ImGui::Checkbox("32-bit accumulation (a)",
                               &settings_.accumulate_in_float32);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      ImGui::SetTooltip(
          "Accumulate the precomputation passes in 32-bit floats before\n"
          "converting the results to half precision. Mostly useful in\n"
          "precomputed luminance mode, with many wavelengths.");
    }
    ImGui::EndDisabled();
    const char* kLuminanceModes[] = {"Off (radiance)", "Approximate",
                                     "Precomputed"};
    int luminance = static_cast<int>(use_luminance_);
    ImGui::BeginDisabled(model_uses_rgb_coefficients_);
    if (ImGui::Combo("Luminance (l)", &luminance, kLuminanceModes, 3)) {
      use_luminance_ = static_cast<Luminance>(luminance);
      changed = true;
    }
    ImGui::EndDisabled();
    if (model_uses_rgb_coefficients_) {
      ImGui::TextDisabled("RGB coefficients: radiance mode only");
    }
    if (changed) {
      model_dirty_ = true;
    }
    if (ImGui::Checkbox("White balance (w)", &do_white_balance_)) {
      UpdateWhitePoint();
    }
  }

  if (ImGui::CollapsingHeader("View", ImGuiTreeNodeFlags_DefaultOpen)) {
    double min_exposure = 0.1;
    double max_exposure = 1000.0;
    ImGui::SliderScalar("Exposure (+/-)", ImGuiDataType_Double, &exposure_,
                        &min_exposure, &max_exposure, "%.2f",
                        ImGuiSliderFlags_Logarithmic);
    double min_distance = 1.0;
    double max_distance = 1e8;
    ImGui::SliderScalar("Distance (m)", ImGuiDataType_Double,
                        &view_distance_meters_, &min_distance, &max_distance,
                        "%.0f", ImGuiSliderFlags_Logarithmic);
    SliderAngle("View zenith", &view_zenith_angle_radians_, 0.0, 90.0);
    SliderAngle("View azimuth", &view_azimuth_angle_radians_, -180.0, 180.0);
    SliderAngle("Sun zenith", &sun_zenith_angle_radians_, 0.0, 180.0);
    SliderAngle("Sun azimuth", &sun_azimuth_angle_radians_, -180.0, 180.0);
    ImGui::TextUnformatted("Predefined views (1-9):");
    for (char key = '1'; key <= '9'; ++key) {
      const char label[2] = {key, '\0'};
      if (key != '1') {
        ImGui::SameLine();
      }
      if (ImGui::Button(label)) {
        HandleKey(key);
      }
    }
  }

  ImGui::Checkbox("LUT viewer (v)", &show_lut_viewer_);
  ImGui::TextDisabled("Mouse: drag, CTRL+drag, wheel: view and sun directions");
  ImGui::TextDisabled("h: show / hide the user interface");
  ImGui::End();
}

void Demo::DrawAtmosphereWindow() {
  const ImVec2 display_size = ImGui::GetIO().DisplaySize;
  ImGui::SetNextWindowPos(ImVec2(display_size.x - 10, 10),
                          ImGuiCond_FirstUseEver, ImVec2(1, 0));
  ImGui::SetNextWindowSize(ImVec2(400, display_size.y * 0.6f),
                           ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Atmosphere")) {
    ImGui::End();
    return;
  }
  AtmosphereSettings& s = settings_;
  bool changed = false;

  ImGui::BeginDisabled(!settings_modified_);
  if (ImGui::Button("Apply")) {
    model_dirty_ = true;
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button("Reset to Earth defaults")) {
    s = AtmosphereSettings();
    changed = true;
  }
  ImGui::PushItemWidth(-160);

  if (ImGui::CollapsingHeader("Planet and Sun",
                              ImGuiTreeNodeFlags_DefaultOpen)) {
    double bottom_radius_km = s.bottom_radius / 1000.0;
    if (InputDouble("Bottom radius (km)", &bottom_radius_km, "%.1f")) {
      s.bottom_radius = bottom_radius_km * 1000.0;
      changed = true;
    }
    double top_radius_km = s.top_radius / 1000.0;
    if (InputDouble("Top radius (km)", &top_radius_km, "%.1f")) {
      s.top_radius = top_radius_km * 1000.0;
      changed = true;
    }
    changed |= InputDouble("Ground albedo", &s.ground_albedo, "%.3f");
    changed |= SliderAngle("Sun angular radius", &s.sun_angular_radius, 0.01,
                           5.0);
    changed |= ImGui::Checkbox("Constant solar spectrum",
                               &s.use_constant_solar_spectrum);
    ImGui::BeginDisabled(!s.use_constant_solar_spectrum);
    changed |= InputDouble("Solar irradiance (W/m2/nm)",
                           &s.constant_solar_irradiance, "%.3f");
    ImGui::EndDisabled();
    changed |= ImGui::Checkbox("Rec.709 band-averaged coefficients (c)",
                               &s.use_rgb_coefficients);
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip(
          "Average the spectral parameters over the linear sRGB channels\n"
          "(weighted by the solar spectrum) instead of sampling them at\n"
          "680, 550 and 440nm. Radiance mode only.");
    }
    if (model_uses_rgb_coefficients_) {
      const char* kNames[] = {"Solar irradiance", "Rayleigh scattering",
                              "Mie scattering", "Mie extinction",
                              "Absorption extinction", "Ground albedo"};
      for (int i = 0; i < 6; ++i) {
        const std::array<double, 3>& rgb = rgb_coefficients_[i];
        ImGui::TextDisabled("%-21s %.4g, %.4g, %.4g", kNames[i], rgb[0],
                            rgb[1], rgb[2]);
      }
    }
  }

  if (ImGui::CollapsingHeader("Rayleigh")) {
    changed |= InputDouble("Scattering at 1um (1/m)", &s.rayleigh, "%.6g");
    changed |= EditDensityProfile("Density profile##rayleigh",
                                  &s.rayleigh_density);
  }

  if (ImGui::CollapsingHeader("Mie")) {
    changed |= InputDouble("Extinction at 1um (1/m)", &s.mie_extinction,
                           "%.6g");
    changed |= InputDouble("Angstrom alpha", &s.mie_angstrom_alpha, "%.3f");
    changed |= InputDouble("Single scattering albedo",
                           &s.mie_single_scattering_albedo, "%.3f");
    changed |= InputDouble("Phase function g", &s.mie_phase_function_g,
                           "%.3f");
    changed |= EditDensityProfile("Density profile##mie", &s.mie_density);
  }

  if (ImGui::CollapsingHeader("Ozone")) {
    changed |= ImGui::Checkbox("Enabled", &s.use_ozone);
    changed |= InputDouble("Amount (Dobson units)", &s.ozone_dobson_units,
                           "%.1f");
    changed |= EditDensityProfile("Density profile##ozone",
                                  &s.absorption_density);
  }

  if (ImGui::CollapsingHeader("Precomputation",
                              ImGuiTreeNodeFlags_DefaultOpen)) {
    changed |= ImGui::InputDouble("Max sun zenith (deg)",
                                  &s.max_sun_zenith_angle_degrees, 0.0, 0.0,
                                  "%.1f");
    changed |= ImGui::SliderInt("Scattering orders",
                                &s.num_scattering_orders, 1, 10);
    const char* kWavelengthRules[] = {"Midpoint (N wavelengths)",
                                      "Fitted, 6 wavelengths",
                                      "Fitted, 9 wavelengths"};
    int wavelength_rule = static_cast<int>(s.wavelength_rule);
    if (ImGui::Combo("Wavelength rule (f)", &wavelength_rule,
                     kWavelengthRules, 3)) {
      s.wavelength_rule = static_cast<WavelengthRule>(wavelength_rule);
      changed = true;
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip(
          "How the spectral precomputations are integrated into sRGB, in\n"
          "precomputed luminance mode. Midpoint: N evenly spaced wavelengths\n"
          "weighted by the color matching functions. Fitted: 6 or 9 optimized\n"
          "wavelengths, with weights fitted to the atmosphere spectra (more\n"
          "accurate colors for Earth-like atmospheres, in 2 or 3 passes\n"
          "instead of N/3; best with 32-bit accumulation). The fitted\n"
          "wavelengths are optimized for the default atmosphere: with very\n"
          "different spectra (e.g. much more ozone), a midpoint rule with\n"
          "30-60 wavelengths can be more accurate. Only used in precomputed\n"
          "luminance mode.");
    }
    ImGui::BeginDisabled(s.wavelength_rule != WavelengthRule::kMidpoint);
    changed |= ImGui::SliderInt("Wavelengths (precomputed luminance)",
                                &s.num_precomputed_wavelengths, 4, 60);
    ImGui::EndDisabled();
    changed |= ImGui::Checkbox("Combined textures", &s.use_combined_textures);
    changed |= ImGui::Checkbox("Half precision", &s.use_half_precision);
  }
  ImGui::PopItemWidth();
  if (changed) {
    settings_modified_ = true;
  }
  ImGui::End();
}

void Demo::DrawLutViewerWindow() {
  ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x / 2, 10),
                          ImGuiCond_FirstUseEver, ImVec2(0.5f, 0));
  if (!ImGui::Begin("LUT viewer", &show_lut_viewer_,
                    ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::End();
    return;
  }
  const char* kTextures[] = {"Transmittance", "Irradiance", "Scattering",
                             "Single Mie scattering"};
  const int num_textures =
      model_ && model_->optional_single_mie_scattering_texture() ? 4 : 3;
  lut_index_ = std::min(lut_index_, num_textures - 1);
  ImGui::Combo("Texture", &lut_index_, kTextures, num_textures);
  const bool is_3d = lut_index_ >= 2;
  ImGui::BeginDisabled(!is_3d);
  ImGui::SliderInt("Slice (r)", &lut_slice_, 0, SCATTERING_TEXTURE_DEPTH - 1);
  ImGui::EndDisabled();
  ImGui::SliderFloat("Scale", &lut_scale_, 1e-3f, 1e3f, "%.3f",
                     ImGuiSliderFlags_Logarithmic);
  ImGui::Checkbox("Alpha channel", &lut_show_alpha_);

  int width = TRANSMITTANCE_TEXTURE_WIDTH;
  int height = TRANSMITTANCE_TEXTURE_HEIGHT;
  if (lut_index_ == 1) {
    width = IRRADIANCE_TEXTURE_WIDTH;
    height = IRRADIANCE_TEXTURE_HEIGHT;
  } else if (is_3d) {
    width = SCATTERING_TEXTURE_WIDTH;
    height = SCATTERING_TEXTURE_HEIGHT;
  }
  // Display the texture with a width of 400 pixels, keeping its aspect ratio.
  const float kDisplayWidth = 400.0f;
  const float display_height = kDisplayWidth * height / width;
  const ImVec2 uv_max(static_cast<float>(width) / SCATTERING_TEXTURE_WIDTH,
                      static_cast<float>(height) / SCATTERING_TEXTURE_HEIGHT);
  ImGui::Image(static_cast<ImTextureID>(
                   descriptor_heap_->gpu_handle(lut_view_texture_.srv()).ptr),
               ImVec2(kDisplayWidth, display_height), ImVec2(0, 0), uv_max);
  ImGui::TextDisabled("%d x %d (r increases upwards)", width, height);
  ImGui::End();
}

void Demo::DrawTimingsWindow() {
  ImGui::SetNextWindowPos(ImVec2(10, ImGui::GetIO().DisplaySize.y - 10),
                          ImGuiCond_FirstUseEver, ImVec2(0, 1));
  if (!ImGui::Begin("Precomputation", nullptr,
                    ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::End();
    return;
  }
  if (model_) {
    double total = 0.0;
    if (ImGui::BeginTable("timings", 2, ImGuiTableFlags_RowBg)) {
      for (const auto& [name, milliseconds] : model_->precompute_timings()) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(name.c_str());
        ImGui::TableNextColumn();
        ImGui::Text("%8.2f ms", milliseconds);
        total += milliseconds;
      }
      ImGui::EndTable();
    }
    ImGui::Separator();
    ImGui::Text("GPU total: %.2f ms", total);
    ImGui::Text("Init total (CPU, incl. compilation): %.0f ms",
                init_time_milliseconds_);
  }
  ImGui::End();
}

void Demo::DrawErrorWindow() {
  if (error_message_.empty()) {
    return;
  }
  const ImVec2 display_size = ImGui::GetIO().DisplaySize;
  ImGui::SetNextWindowPos(ImVec2(display_size.x / 2, display_size.y / 2),
                          ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(600, 300), ImGuiCond_FirstUseEver);
  if (ImGui::Begin("Shader error")) {
    if (ImGui::Button("Dismiss")) {
      error_message_.clear();
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
    ImGui::TextWrapped("%s", error_message_.c_str());
    ImGui::PopStyleColor();
  }
  ImGui::End();
}

}  // namespace demo
}  // namespace atmosphere
