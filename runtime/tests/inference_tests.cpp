#include "sof/inference.hpp"

#include "sof/dataset.hpp"
#include "sof/lip.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

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
    return frame;
  }

  sof::Frame read_pgm(const char* path)
  {
    std::ifstream file(path, std::ios::binary);
    std::string magic;
    std::uint32_t maximum {};
    sof::Frame frame;
    file >> magic >> frame.width >> frame.height >> maximum;
    require(file && magic == "P5" && frame.width == sof::capture_width && frame.height == sof::capture_height && maximum == 255,
      "benchmark input must be a binary 400x400 PGM");
    file.get();
    frame.luma.resize(static_cast<std::size_t>(frame.width) * frame.height);
    file.read(reinterpret_cast<char*>(frame.luma.data()), static_cast<std::streamsize>(frame.luma.size()));
    require(file.gcount() == static_cast<std::streamsize>(frame.luma.size()), "benchmark PGM is incomplete");
    return frame;
  }

  double benchmark(sof::Inference& inference, const sof::Frame& frame,
    std::array<sof::Parameter, sof::max_parameters>& parameters)
  {
    constexpr int warmup = 50;
    constexpr int iterations = 300;
    for (int i = 0; i < warmup; ++i)
      inference.run(frame, parameters);
    const auto begin = std::chrono::steady_clock::now();
    for (int i = 0; i < iterations; ++i)
      inference.run(frame, parameters);
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    return std::chrono::duration<double, std::milli>(elapsed).count() / iterations;
  }

  void test_median_filter()
  {
    for (const auto width : { 1, 2, 7, 31, 800 })
    {
      for (const auto height : { 1, 2, 9, 23, 400 })
      {
        std::vector<std::uint8_t> input(static_cast<std::size_t>(width * height));
        std::vector<std::uint8_t> filtered(input.size());
        std::vector<std::uint8_t> separable(input.size()), scratch(input.size());
        std::uint32_t seed = 713;
        for (auto& value : input)
        {
          seed = seed * 1664525U + 1013904223U;
          value = static_cast<std::uint8_t>(seed >> 24);
        }
        sof::median_filter_5x5(input, filtered, static_cast<std::size_t>(width),
          static_cast<std::size_t>(height));
        sof::pseudo_separable_median_5x5(input, separable, scratch, static_cast<std::size_t>(width),
          static_cast<std::size_t>(height));
        for (int y = 0; y < height; ++y)
        {
          for (int x = 0; x < width; ++x)
          {
            std::array<std::uint8_t, 25> neighbors {};
            std::array<std::uint8_t, 5> row_medians {};
            std::size_t i {};
            for (int dy = -2; dy <= 2; ++dy)
            {
              std::array<std::uint8_t, 5> row {};
              for (int dx = -2; dx <= 2; ++dx)
                row[static_cast<std::size_t>(dx + 2)] = neighbors[i++] =
                  input[static_cast<std::size_t>(std::clamp(y + dy, 0, height - 1) * width + std::clamp(x + dx, 0, width - 1))];
              std::sort(row.begin(), row.end());
              row_medians[static_cast<std::size_t>(dy + 2)] = row[2];
            }
            std::sort(row_medians.begin(), row_medians.end());
            require(separable[static_cast<std::size_t>(y * width + x)] == row_medians[2],
              "separable median differs from sorted row medians with replicated borders");
            std::sort(neighbors.begin(), neighbors.end());
            require(filtered[static_cast<std::size_t>(y * width + x)] == neighbors[12],
              "median filter differs from a full 5x5 sort with replicated borders");
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

int main(int argc, char** argv)
{
  try
  {
    require(argc == 2 || argc == 3, "expected lip model path and optional benchmark PGM/dataset");
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
    require(std::abs(raw_output[0] - 0.2056102F) < 0.0001F && std::abs(raw_output[3] - 1.1964159F) < 0.0001F && std::abs(raw_output[12] - 0.9483699F) < 0.0001F && std::abs(raw_output[24] - 2.2418664F) < 0.0001F,
      "lip model graph output changed");
    for (int i = 0; i < 30; ++i)
      require(inference->run(frame, parameters) == 37, "unexpected lip output count");
    require(std::string_view(parameters[0].name.data()) == "jaw_right", "unexpected first output");
    require(std::string_view(parameters[36].name.data()) == "tongue_down_left_morph",
      "unexpected last output");
    for (std::size_t i = 0; i < 37; ++i)
    {
      require(std::isfinite(parameters[i].value), "non-finite lip output");
      require(parameters[i].value >= 0.0F, "negative lip output");
    }
    inference.reset();

    if (argc == 3)
    {
      if (std::filesystem::path(argv[2]).extension() == ".sofdata")
      {
        sof::DatasetReader dataset(argv[2]);
        require(dataset.read(frame), "benchmark dataset is empty");
      }
      else
      {
        frame = read_pgm(argv[2]);
      }
    }

    auto normal = sof::open_lip_mnn(config, sof::InferencePrecision::normal);
    std::array<sof::Parameter, sof::max_parameters> normal_parameters {}, low_parameters {};
    const double normal_ms = benchmark(*normal, frame, normal_parameters);
    normal.reset();
    auto low = sof::open_lip_mnn(config, sof::InferencePrecision::low);
    const double low_ms = benchmark(*low, frame, low_parameters);
    double absolute_sum {};
    double maximum_error {};
    for (std::size_t i = 0; i < sof::lip_parameter_count; ++i)
    {
      const double error = std::abs(static_cast<double>(normal_parameters[i].value) - static_cast<double>(low_parameters[i].value));
      absolute_sum += error;
      maximum_error = std::max(maximum_error, error);
    }
    std::cout << std::fixed << std::setprecision(6) << "lip inference contract passed\n"
              << "normal: " << normal_ms << " ms, low: " << low_ms
              << " ms, speedup: " << normal_ms / low_ms << "x\n"
              << "output MAE: " << absolute_sum / sof::lip_parameter_count
              << ", max error: " << maximum_error << '\n';
    return 0;
  }
  catch (const std::exception& error)
  {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
