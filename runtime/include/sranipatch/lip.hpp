#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace sranipatch
{
  void pseudo_separable_median_5x5(std::span<const std::uint8_t> input, std::span<std::uint8_t> output,
    std::span<std::uint8_t> scratch, std::size_t width, std::size_t height);

  inline constexpr std::size_t lip_raw_count = 30;
  inline constexpr std::size_t lip_parameter_count = 37;
  inline constexpr std::array<std::string_view, lip_parameter_count> lip_parameter_names {
    "jaw_right",
    "jaw_left",
    "jaw_forward",
    "jaw_open",
    "mouth_ape_shape",
    "mouth_upper_right",
    "mouth_upper_left",
    "mouth_lower_right",
    "mouth_lower_left",
    "mouth_upper_overturn",
    "mouth_lower_overturn",
    "mouth_pout",
    "mouth_smile_right",
    "mouth_smile_left",
    "mouth_sad_right",
    "mouth_sad_left",
    "cheek_puff_right",
    "cheek_puff_left",
    "cheek_suck",
    "mouth_upper_up_right",
    "mouth_upper_up_left",
    "mouth_lower_down_right",
    "mouth_lower_down_left",
    "mouth_upper_inside",
    "mouth_lower_inside",
    "mouth_lower_overlay",
    "tongue_long_step_1",
    "tongue_left",
    "tongue_right",
    "tongue_up",
    "tongue_down",
    "tongue_roll",
    "tongue_long_step_2",
    "tongue_up_right_morph",
    "tongue_up_left_morph",
    "tongue_down_right_morph",
    "tongue_down_left_morph",
  };

  class LipPostprocessor
  {
  public:
    LipPostprocessor();
    std::array<float, lip_parameter_count> process(std::span<const float, lip_raw_count> raw);

  private:
    void reset_tracking();

    bool was_valid_ { true };
    unsigned valid_streak_ {};
    unsigned invalid_streak_ {};
    unsigned calibration_tick_ {};
    std::array<float, 4> centers_ {};
    std::array<float, 4> center_estimates_ {};
    std::array<float, 4> center_weights_ {};
    std::array<float, 36> maxima_ {};
    std::array<float, 33> smoothed_ {};
    std::array<float, 33> smoothing_weights_ {};
  };
}
