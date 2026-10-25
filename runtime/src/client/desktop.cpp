#include "desktop.hpp"

#include "style.hpp"

#include <glad/gl.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <implot.h>

#include <cstdio>
#include <exception>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <vector>

namespace sranipatch
{
  struct Desktop::Impl
  {
    GLFWwindow* window {};
    bool glfw {}, imgui {}, implot {}, glfw_backend {}, opengl_backend {}, rendering {};
    std::vector<char> font_data;
    std::function<void()> draw;
    std::exception_ptr error;

    ~Impl()
    {
      detach();
      if (window)
        glfwMakeContextCurrent(window);
      if (opengl_backend)
        ImGui_ImplOpenGL3_Shutdown();
      if (glfw_backend)
        ImGui_ImplGlfw_Shutdown();
      if (implot)
        ImPlot::DestroyContext();
      if (imgui)
        ImGui::DestroyContext();
      if (window)
        glfwDestroyWindow(window);
      if (glfw)
        glfwTerminate();
    }

    void initialize(const std::filesystem::path& font)
    {
      std::ifstream file(font, std::ios::binary);
      font_data.assign(std::istreambuf_iterator<char> { file }, {});
      if (file.bad() || font_data.empty())
        throw std::runtime_error("Cannot read bundled font: " + font.string());
      glfwSetErrorCallback([](int, const char* message) { std::cerr << "GLFW: " << message << '\n'; });
      glfw = glfwInit();
      if (!glfw)
        throw std::runtime_error("GLFW initialization failed");
      glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
      glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
      glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
      window = glfwCreateWindow(900, 720, "SRanipatch", nullptr, nullptr);
      if (!window)
        throw std::runtime_error("OpenGL 3.3 window creation failed");
      glfwMakeContextCurrent(window);
      if (!gladLoadGL(glfwGetProcAddress) || !GLAD_GL_VERSION_3_3)
        throw std::runtime_error("OpenGL 3.3 loader initialization failed");
      glfwSwapInterval(1);
      IMGUI_CHECKVERSION();
      ImGui::CreateContext();
      imgui = true;
      ImPlot::CreateContext();
      implot = true;
      ImGui::GetIO().IniFilename = nullptr;
      set_dark_pastel_style();
      ImFontConfig config;
      config.FontDataOwnedByAtlas = false;
      std::snprintf(config.Name, sizeof(config.Name), "Quicksand Medium");
      if (!ImGui::GetIO().Fonts->AddFontFromMemoryTTF(font_data.data(), static_cast<int>(font_data.size()),
            18.0f, &config))
        throw std::runtime_error("Cannot load bundled Quicksand font");
      glfw_backend = ImGui_ImplGlfw_InitForOpenGL(window, true);
      if (!glfw_backend)
        throw std::runtime_error("GUI window initialization failed");
      opengl_backend = ImGui_ImplOpenGL3_Init("#version 330");
      if (!opengl_backend)
        throw std::runtime_error("GUI renderer initialization failed");
    }

    void detach()
    {
      if (window)
      {
        glfwSetWindowRefreshCallback(window, nullptr);
        glfwSetFramebufferSizeCallback(window, nullptr);
        glfwSetWindowUserPointer(window, nullptr);
      }
      draw = {};
    }

    void frame(bool refresh)
    {
      if (rendering || error)
        return;
      int width {}, height {};
      glfwGetFramebufferSize(window, &width, &height);
      if (width <= 0 || height <= 0)
        return;
      rendering = true;
      try
      {
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        draw();
        ImGui::Render();
        glfwGetFramebufferSize(window, &width, &height);
        glViewport(0, 0, width, height);
        glClearColor(0.08F, 0.09F, 0.11F, 1.0F);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
        if (refresh)
          glFinish();
      }
      catch (...)
      {
        rendering = false;
        throw;
      }
      rendering = false;
    }

    void run(const std::function<void()>& render, const volatile std::sig_atomic_t& interrupted)
    {
      draw = render;
      error = {};
      glfwSetWindowUserPointer(window, this);
      // GLFW fixes the order and types of the framebuffer dimensions.
      // NOLINTBEGIN(bugprone-easily-swappable-parameters)
      glfwSetFramebufferSizeCallback(
        window, [](GLFWwindow*, int width, int height) { glViewport(0, 0, width, height); }
      );
      // NOLINTEND(bugprone-easily-swappable-parameters)
      glfwSetWindowRefreshCallback(window, [](GLFWwindow* window) {
        auto& self = *static_cast<Impl*>(glfwGetWindowUserPointer(window));
        try
        {
          self.frame(true);
        }
        catch (...)
        {
          self.error = std::current_exception();
        }
      });
      try
      {
        while (!interrupted && !glfwWindowShouldClose(window))
        {
          glfwPollEvents();
          if (error)
            std::rethrow_exception(error);
          if (!interrupted && !glfwWindowShouldClose(window))
            frame(false);
        }
      }
      catch (...)
      {
        detach();
        throw;
      }
      detach();
    }
  };

  Desktop::Desktop(const std::filesystem::path& font) : impl_(std::make_unique<Impl>())
  {
    impl_->initialize(font);
  }

  Desktop::~Desktop() = default;

  void Desktop::run(const std::function<void()>& draw, const volatile std::sig_atomic_t& interrupted)
  {
    impl_->run(draw, interrupted);
  }
}  // namespace sranipatch
