#include "head_model.hpp"
#include "sranipatch/lip.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>

namespace
{
  void require(bool condition, const char* message)
  {
    if (!condition)
      throw std::runtime_error(message);
  }
}  // namespace

// Exceptions from the body are caught below; clang-tidy misidentifies MSVC exception inheritance.
// NOLINTNEXTLINE(bugprone-exception-escape)
int main(int argc, char** argv)
{
  try
  {
    require(argc == 2, "Expected head model path");
    sranipatch::HeadModel model(argv[1]);
    require(!model.vertices().empty() && !model.indices().empty(), "Head geometry is missing");
    const std::vector<sranipatch::HeadVertex> neutral(model.vertices().begin(), model.vertices().end());
    std::array<sranipatch::Parameter, sranipatch::lip_parameter_count> parameters {};
    for (std::size_t shape = 0; shape < parameters.size(); ++shape)
    {
      parameters.fill({});
      parameters[shape].value = 1;
      model.deform(parameters);
      bool moved {};
      for (std::size_t i = 0; i < neutral.size(); ++i)
      {
        const auto& vertex = model.vertices()[i];
        for (std::size_t axis = 0; axis < 3; ++axis)
        {
          require(std::isfinite(vertex.position[axis]), "Non-finite morphed position");
          moved |= std::abs(vertex.position[axis] - neutral[i].position[axis]) > 1e-6F;
        }
        float length {};
        for (const auto component : vertex.normal)
          length += component * component;
        require(std::abs(length - 1) < 1e-4F, "Morphed normal is not normalized");
      }
      if (!moved)
        throw std::runtime_error("Blendshape did not deform geometry: " + std::string(sranipatch::lip_parameter_names[shape]));
    }
    parameters.fill({});
    parameters[3].value = 0.5F;
    model.deform(parameters);
    const std::vector<sranipatch::HeadVertex> half(model.vertices().begin(), model.vertices().end());
    parameters[3].value = 1;
    model.deform(parameters);
    for (std::size_t i = 0; i < neutral.size(); ++i)
      for (std::size_t axis = 0; axis < 3; ++axis)
        require(std::abs(half[i].position[axis] - (neutral[i].position[axis] + model.vertices()[i].position[axis]) * 0.5F) < 1e-5F,
          "Blendshape weights are not linear");
    parameters[3].value = 0;
    model.deform(parameters);
    for (std::size_t i = 0; i < neutral.size(); ++i)
      require(model.vertices()[i].position == neutral[i].position, "Morphs accumulate across frames");
    std::cout << "All 37 lip blendshapes deform correctly by index\n";
    return 0;
  }
  catch (const std::exception& error)
  {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
