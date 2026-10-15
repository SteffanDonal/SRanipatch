#include "sof/inference.hpp"

#include "sof/lip.hpp"

#include <cmath>
#include <iostream>
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
}  // namespace

int main(int argc, char** argv)
{
  try
  {
    require(argc == 2, "expected lip model path");

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

    std::array<sof::Parameter, sof::max_parameters> parameters {};
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
    std::cout << "lip inference contract passed\n";
    return 0;
  }
  catch (const std::exception& error)
  {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
