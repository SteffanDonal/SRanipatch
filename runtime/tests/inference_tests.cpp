#include "sof/inference.hpp"

#include "sof/lip.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
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
    file.read(reinterpret_cast<char*>(frame.luma.data()),
      static_cast<std::streamsize>(frame.luma.size()));
    require(file.gcount() == static_cast<std::streamsize>(frame.luma.size()),
      "benchmark PGM is incomplete");
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
}  // namespace

int main(int argc, char** argv)
{
  try
  {
    require(argc == 2 || argc == 3, "expected lip model path and optional benchmark PGM");

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
    const auto frame = argc == 3 ? read_pgm(argv[2]) : test_frame();

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
    std::cout << std::fixed << std::setprecision(6)
              << "lip inference contract passed\n"
              << "normal: " << normal_ms << " ms, low: " << low_ms << " ms, speedup: "
              << normal_ms / low_ms << "x\n"
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
