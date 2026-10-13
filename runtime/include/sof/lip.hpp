#pragma once

#include <array>
#include <cstddef>
#include <span>

namespace sof
{
  inline constexpr std::size_t lip_raw_count = 30;
  inline constexpr std::size_t lip_parameter_count = 37;

  class LipPostprocessor
  {
  public:
    LipPostprocessor();
    std::array<float, lip_parameter_count> process(std::span<const float, lip_raw_count> raw);

  private:
    void reset_tracking();

    bool was_valid_ {};
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
}  // namespace sof
