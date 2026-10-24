#include "head_preview.hpp"
#include "sof/lip.hpp"

#include <GLFW/glfw3.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace
{
  void save_ppm(const std::filesystem::path& path, std::span<const unsigned char> pixels, int size)
  {
    std::ofstream image(path, std::ios::binary);
    image << "P6\n"
          << size << ' ' << size << "\n255\n";
    for (int y = size - 1; y >= 0; --y)
      for (int x = 0; x < size; ++x)
        image.write(reinterpret_cast<const char*>(&pixels[static_cast<std::size_t>(y * size + x) * 4]),
          3);
    if (!image)
      throw std::runtime_error("Cannot write blendshape preview: " + path.string());
  }
}  // namespace

int main(int argc, char** argv)
{
  GLFWwindow* window {};
  try
  {
    if (argc < 2 || !glfwInit())
      throw std::runtime_error("Expected model path and a working graphics session");
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    window = glfwCreateWindow(128, 128, "Head renderer test", nullptr, nullptr);
    if (!window)
      throw std::runtime_error("Cannot create hidden OpenGL context");
    glfwMakeContextCurrent(window);
    {
      sof::HeadPreview preview(argv[1]);
      const int size = argc == 5 ? 512 : 256;
      const auto read = [&](std::span<const sof::Parameter> parameters) {
        const auto texture = preview.render(parameters, size, size);
        glBindTexture(GL_TEXTURE_2D, texture);
        std::vector<unsigned char> pixels(static_cast<std::size_t>(size * size * 4));
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        if (glGetError() != GL_NO_ERROR)
          throw std::runtime_error("OpenGL error while rendering head preview");
        return pixels;
      };
      const auto neutral = read({});
      std::size_t gray_pixels {};
      for (std::size_t i = 0; i < neutral.size(); i += 4)
      {
        if (neutral[i] == neutral[i + 1] && neutral[i] == neutral[i + 2] && neutral[i] > 40)
          ++gray_pixels;
      }
      if (gray_pixels < 1000)
        throw std::runtime_error("Head preview did not produce visible grayscale geometry");
      std::array<sof::Parameter, sof::lip_parameter_count> parameters {};
      parameters[3].value = 1;
      if (read(parameters) == neutral)
        throw std::runtime_error("Jaw blendshape did not change rendered output");
      const auto wide_texture = preview.render({}, 256, 128);
      glBindTexture(GL_TEXTURE_2D, wide_texture);
      GLint width {}, height {};
      glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
      glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
      if (width != 256 || height != 128 || glGetError() != GL_NO_ERROR)
        throw std::runtime_error("Cannot render a 2:1 preview viewport");
      if (argc == 3)
        save_ppm(argv[2], neutral, size);
      if (argc == 5 && std::string_view(argv[2]) == "--export")
      {
        const std::filesystem::path directory(argv[3]);
        std::filesystem::create_directories(directory);
        save_ppm(directory / "neutral.ppm", neutral, size);
        std::ifstream names(argv[4]);
        if (!names)
          throw std::runtime_error("Cannot open blendshape names");
        std::string name;
        std::size_t shape {};
        while (std::getline(names, name))
        {
          if (name.empty())
            continue;
          if (name.find_first_of("/\\") != std::string::npos || shape >= parameters.size())
            throw std::runtime_error("Invalid blendshape name");
          parameters.fill({});
          parameters[shape++].value = 1.0F;
          save_ppm(directory / (name + ".ppm"), read(parameters), size);
          std::cout << "Rendered " << name << " = 1.0\n";
        }
      }
    }
    glfwDestroyWindow(window);
    glfwTerminate();
    std::cout << "Head renderer and live jaw deformation passed\n";
    return 0;
  }
  catch (const std::exception& error)
  {
    std::cerr << error.what() << '\n';
    if (window)
      glfwDestroyWindow(window);
    glfwTerminate();
    return 1;
  }
}
