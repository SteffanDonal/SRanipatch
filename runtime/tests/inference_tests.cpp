#include "sof/inference.hpp"

#include "sof/lip.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
  void require(bool condition, std::string_view message)
  {
    if (!condition)
      throw std::runtime_error(std::string(message));
  }

  sof::Frame test_frame()
  {
    sof::Frame frame;
    frame.width = sof::capture_width;
    frame.height = sof::capture_height;
    frame.luma.resize(static_cast<std::size_t>(frame.width) * frame.height);
    for (std::uint32_t y = 0; y < frame.height; ++y)
    {
      for (std::uint32_t x = 0; x < frame.width; ++x)
      {
        const auto value = static_cast<std::uint8_t>((x + y * 3) % 256);
        frame.luma[static_cast<std::size_t>(y) * frame.width + x] = value;
      }
    }
    frame.yuyv.resize(static_cast<std::size_t>(frame.width) * frame.height * 2);
    for (std::size_t i = 0; i < frame.yuyv.size(); ++i)
      frame.yuyv[i] = static_cast<std::uint8_t>((i % 800 + (i / 800) * 3) % 256);
    return frame;
  }

  void test_median_filter()
  {
    for (const auto width : { 1, 2, 7, 31, 800 })
    {
      for (const auto height : { 1, 2, 9, 23, 400 })
      {
        std::vector<std::uint8_t> input(static_cast<std::size_t>(width) * height);
        std::vector<std::uint8_t> separable(input.size()), scratch(input.size());
        std::uint32_t seed = 713;
        for (auto& value : input)
        {
          seed = seed * 1664525U + 1013904223U;
          value = static_cast<std::uint8_t>(seed >> 24);
        }
        sof::pseudo_separable_median_5x5(input, separable, scratch, static_cast<std::size_t>(width),
          static_cast<std::size_t>(height));
        for (int y = 0; y < height; ++y)
        {
          for (int x = 0; x < width; ++x)
          {
            std::array<std::uint8_t, 5> row_medians {};
            for (int dy = -2; dy <= 2; ++dy)
            {
              std::array<std::uint8_t, 5> row {};
              for (int dx = -2; dx <= 2; ++dx)
                row[dx + 2] =
                  input[static_cast<std::size_t>(std::clamp(y + dy, 0, height - 1)) * width + std::clamp(x + dx, 0, width - 1)];
              std::sort(row.begin(), row.end());
              row_medians[dy + 2] = row[2];
            }
            std::sort(row_medians.begin(), row_medians.end());
            require(separable[static_cast<std::size_t>(y) * width + x] == row_medians[2],
              "separable median differs from sorted row medians with replicated borders");
          }
        }
      }
    }
  }

  void test_tracking_and_tongue()
  {
    sof::LipPostprocessor postprocessor;
    std::array<float, sof::lip_raw_count> raw {};
    raw[0] = 1.0F;
    raw[1] = raw[5] = raw[6] = 0.5F;
    raw[3] = 0.2F;
    raw[28] = 0.3F;
    for (int i = 0; i < 25; ++i)
    {
      const auto output = postprocessor.process(raw);
      require(std::all_of(output.begin(), output.end(), [](float value) { return value == 0; }),
        "tracking must wait for 26 initially valid frames");
    }
    auto output = postprocessor.process(raw);
    require(std::abs(output[32] - 1.0F / 6.0F) < 1e-6F && output[26] == 1.0F,
      "tongue-down length offset or initial tracking gate differs from SRanipal");
    raw[28] = 0.0F;
    for (int i = 0; i < 2; ++i)
      output = postprocessor.process(raw);
    require(std::abs(output[26] - 0.0930232555F) < 1e-6F,
      "directional tongue length did not enter the smoothing state");
    raw[0] = 0.0F;
    for (int i = 0; i < 30; ++i)
      output = postprocessor.process(raw);
    raw[0] = 1.0F;
    for (int i = 0; i < 26; ++i)
    {
      output = postprocessor.process(raw);
      require(output[3] == 0.0F, "tracking reacquired before 27 valid frames");
    }
    output = postprocessor.process(raw);
    require(std::abs(output[3] - 0.133333333F) < 1e-6F, "tracking reset did not clear smoothing state");
  }

  void test_calibration_reset()
  {
    // Original SRanipal checkpoints after brief losses and full tracking resets.
    struct Checkpoint
    {
      unsigned frame;
      std::array<float, 7> values;
    };

    constexpr std::array<Checkpoint, 3> reference { {
      { 592, { 0.0F, 1.0F, 0.00275099883F, 5.54795498e-8F, 0.398974419F, 3.40670352e-12F, 0.848515809F } },
      { 604, { 0.0F, 0.0625F, 6.71630573e-7F, 1.3544812e-11F, 0.0362179503F, 0.0521963313F, 0.00763657875F } },
      { 1192,
        { 9.82753629e-15F, 0.471414298F, 0.000686350395F, 0.371601224F, 9.14199161e-10F, 4.18876382e-8F,
          0.482305378F } },
    } };
    constexpr std::array<std::size_t, 7> indices { 0, 1, 2, 5, 6, 7, 8 };
    sof::LipPostprocessor postprocessor;
    for (unsigned frame = 0; frame < 1200; ++frame)
    {
      std::array<float, sof::lip_raw_count> raw {};
      for (std::size_t i = 0; i < raw.size(); ++i)
        raw[i] = static_cast<float>((frame * (i * 2 + 1) + i * 37) % 997) / 997.0F;
      const auto phase = frame % 600;
      raw[0] = (phase >= 240 && phase < 249) || (phase >= 400 && phase < 440) ? 0.2F : 0.8F;
      if (phase < 200)
        raw[1] = raw[5] = raw[6] = 0.5F;
      const auto output = postprocessor.process(raw);
      for (const auto& checkpoint : reference)
        if (frame + 1 == checkpoint.frame)
          for (std::size_t i = 0; i < indices.size(); ++i)
            require(std::abs(output[indices[i]] - checkpoint.values[i]) < 1e-6F,
              "calibration across tracking loss differs from SRanipal");
    }
  }
}  // namespace

// Exceptions from the body are caught below; clang-tidy misidentifies MSVC exception inheritance.
// NOLINTNEXTLINE(bugprone-exception-escape)
int main(int argc, char** argv)
{
  try
  {
    require(argc == 2, "expected lip model path");
    test_median_filter();
    test_tracking_and_tongue();
    test_calibration_reset();

    sof::LipPostprocessor postprocessor;
    std::array<float, sof::lip_raw_count> raw {};
    raw[0] = 1.0F;
    raw[1] = 0.25F;
    raw[2] = 0.8F;
    raw[3] = 0.8F;
    raw[5] = 0.25F;
    raw[6] = 0.75F;
    std::array<float, sof::lip_parameter_count> shaped {};
    for (int i = 0; i < 30; ++i)
      shaped = postprocessor.process(raw);
    require(shaped[0] > 0.0F && shaped[1] == 0.0F, "jaw axis split is wrong");
    require(shaped[5] > 0.0F && shaped[6] == 0.0F, "upper-mouth axis split is wrong");
    require(shaped[7] == 0.0F && shaped[8] > 0.0F, "lower-mouth axis split is wrong");
    require(shaped[2] > 0.0F && shaped[3] > 0.0F, "direct lip shapes are missing");

    sof::Config config;
    sof::set_text(config.model, argv[1]);
    auto inference = sof::open_lip_mnn(config);
    sof::Frame frame = test_frame();

    std::array<sof::Parameter, sof::max_parameters> parameters {};
    std::array<float, sof::lip_raw_count> raw_output {};
    inference->run(frame, parameters, raw_output);
    constexpr std::array<float, sof::lip_raw_count> expected { 0.858044505F,
      0.27993238F,
      0,
      0.910982132F,
      0,
      0.405989289F,
      0.274118125F,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0.846034288F,
      0.730406106F,
      0,
      0,
      0,
      2.23130155F,
      0,
      1.15679634F,
      0,
      0.118545368F,
      1.35244107F };
    for (std::size_t i = 0; i < expected.size(); ++i)
      require(std::abs(raw_output[i] - expected[i]) < 0.0001F, "lip model graph output changed");
    for (const auto size : { std::size_t { 0 }, frame.yuyv.size() - 1, frame.yuyv.size() + 1 })
    {
      auto invalid = frame;
      invalid.yuyv.resize(size);
      bool rejected {};
      try
      {
        inference->run(invalid, parameters);
      }
      catch (const std::runtime_error&)
      {
        rejected = true;
      }
      require(rejected, "missing or malformed packed bytes accepted");
    }
    auto wrong_dimensions = frame;
    --wrong_dimensions.width;
    bool rejected_dimensions {};
    try
    {
      inference->run(wrong_dimensions, parameters);
    }
    catch (const std::runtime_error&)
    {
      rejected_dimensions = true;
    }
    require(rejected_dimensions, "incorrect packed frame dimensions accepted");
    for (int i = 0; i < 30; ++i)
      require(inference->run(frame, parameters) == 37, "unexpected lip output count");
    require(std::string_view(parameters[0].name.data()) == "jaw_right", "unexpected first output");
    require(std::string_view(parameters[36].name.data()) == "tongue_down_left_morph",
      "unexpected last output");
    for (std::size_t i = 0; i < 37; ++i)
    {
      require(std::isfinite(parameters[i].value), "non-finite lip output");
      require(parameters[i].value >= 0.0F && parameters[i].value <= 2.0F,
        "lip output exceeds fixture bounds");
    }
    inference.reset();

    auto low = sof::open_lip_mnn(config, sof::InferencePrecision::low);
    std::array<float, sof::lip_raw_count> low_raw {};
    frame.luma.clear();
    for (int iteration = 0; iteration < 31; ++iteration)
      require(low->run(frame, parameters, low_raw) == sof::lip_parameter_count,
        "unexpected low precision output count");
    for (std::size_t i = 0; i < expected.size(); ++i)
      require(std::isfinite(low_raw[i]) && std::abs(low_raw[i] - expected[i]) < 0.05F,
        "low precision model output exceeds tolerance");
    for (std::size_t i = 0; i < sof::lip_parameter_count; ++i)
      require(std::isfinite(parameters[i].value) && parameters[i].value >= 0 && parameters[i].value <= 2,
        "low precision shape exceeds fixture bounds");
    std::cout << "lip inference contract passed\n";
    return 0;
  }
  catch (const std::exception& error)
  {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
