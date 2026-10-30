#include "sranipatch/lip.hpp"

#include <algorithm>
#include <cmath>
#include <span>

namespace sranipatch
{
  namespace
  {
    constexpr float validity_threshold = 0.4F;
    constexpr unsigned stable_frames = 25;

    void split_axis(float value, float center, std::span<float, 2> output)
    {
      auto& negative = output[0];
      auto& positive = output[1];
      if (value < center)
      {
        negative = center > 0.0F ? (center - value) / center : 0.0F;
        positive = 0.0F;
      }
      else
      {
        negative = 0.0F;
        positive = center < 1.0F ? (value - center) / (1.0F - center) : 0.0F;
      }
    }
  }  // namespace

  LipPostprocessor::LipPostprocessor()
  {
    reset_tracking();
  }

  void LipPostprocessor::reset_tracking()
  {
    centers_.fill(0.5F);
    center_estimates_.fill(0.5F);
    center_weights_.fill(0.5F);
    maxima_.fill(0.5F);
    maxima_[2] = 0.7F;
    smoothed_.fill(0.0F);
    smoothing_weights_.fill(0.0F);
  }

  std::array<float, lip_parameter_count> LipPostprocessor::process(std::span<const float, lip_raw_count> raw)
  {
    std::array<float, lip_parameter_count> output {};
    const bool valid = raw[0] >= validity_threshold;
    if (valid == was_valid_)
    {
      if (valid)
        ++valid_streak_;
      else
        ++invalid_streak_;
    }
    else
    {
      valid_streak_ = 0;
      invalid_streak_ = 0;
    }
    was_valid_ = valid;

    if (!valid)
    {
      if (invalid_streak_ > stable_frames)
        reset_tracking();
      return output;
    }
    if (valid_streak_ <= stable_frames)
      return output;

    const std::array<float, 4> center_inputs { raw[1], raw[5], raw[6], raw[2] };
    if (valid_streak_ > 150 && ++calibration_tick_ > 12)
    {
      calibration_tick_ = 0;
      for (std::size_t i = 0; i < centers_.size(); ++i)
      {
        const float difference = std::abs(center_inputs[i] - centers_[i]);
        if (difference >= 0.2F && !(i == 3 && difference < 0.55F && center_inputs[i] < 0.9F))
          continue;
        center_weights_[i] += 0.001F;
        const float alpha = center_weights_[i] / (center_weights_[i] + 20.5F);
        center_estimates_[i] = std::lerp(center_estimates_[i], center_inputs[i], alpha);
        center_weights_[i] *= 1.0F - alpha;
        centers_[i] = center_estimates_[i];
      }
    }

    split_axis(raw[1], centers_[0], std::span(output).subspan<0, 2>());
    output[2] = std::max(0.0F, raw[2] - centers_[3]);
    output[3] = raw[3];
    output[4] = raw[4];
    split_axis(raw[5], centers_[1], std::span(output).subspan<5, 2>());
    split_axis(raw[6], centers_[2], std::span(output).subspan<7, 2>());
    std::copy(raw.begin() + 7, raw.end(), output.begin() + 9);

    for (std::size_t i = 0; i < maxima_.size(); ++i)
    {
      maxima_[i] = std::max(maxima_[i], output[i]);
      if ((i <= 26 || maxima_[i] >= 0.502F) && maxima_[i] > 0.0F)
        output[i] /= maxima_[i];
    }

    if (output[31] > 0.4F)
    {
      output[26] *= 1.5F;
      std::fill(output.begin() + 27, output.begin() + 31, 0.0F);
    }
    if (output[30] > output[29])
      output[29] = 0.0F;
    else
      output[30] = 0.0F;
    if (output[27] > output[28])
      output[28] = 0.0F;
    else
      output[27] = 0.0F;

    if (output[30] > 0.1F)
    {
      output[32] = output[30] + 0.2F + 0.2F * (output[27] + output[28]);
      output[26] = 1.0F;
    }
    else if (output[29] > 0.2F && output[3] < 0.6F && output[26] > 0.2F)
    {
      output[32] = output[29] + 0.12F + 0.2F * (output[27] + output[28]);
      output[26] = 1.0F;
    }
    else if (output[27] > 0.2F && output[3] < 0.6F && output[26] > 0.3F)
    {
      output[32] = output[27] + 0.12F + 0.2F * (output[29] + output[28]);
      output[26] = 1.0F;
    }
    else if (output[28] > 0.2F && output[3] < 0.6F && output[26] > 0.3F)
    {
      output[32] = output[28] + 0.12F + 0.2F * (output[29] + output[27]);
      output[26] = 1.0F;
    }
    else if (output[26] < 0.4F)
    {
      output[26] /= 0.4F;
    }
    else
    {
      output[32] = (output[26] - 0.4F) / 0.6F;
      output[26] = 1.0F;
    }
    if (output[3] < 0.05F)
      std::fill(output.begin() + 26, output.begin() + 33, 0.0F);

    for (std::size_t i = 0; i < smoothed_.size(); ++i)
    {
      smoothing_weights_[i] += 0.25F;
      const float alpha = smoothing_weights_[i] / (smoothing_weights_[i] + 0.5F);
      smoothed_[i] = std::lerp(smoothed_[i], output[i], alpha);
      smoothing_weights_[i] *= 1.0F - alpha;
      output[i] = smoothed_[i];
    }
    if (output[32] > 0.05F)
      output[26] = 1.0F;
    output[33] = output[28] * output[29];
    output[34] = output[27] * output[29];
    output[35] = output[28] * output[30];
    output[36] = output[27] * output[30];
    return output;
  }
}
