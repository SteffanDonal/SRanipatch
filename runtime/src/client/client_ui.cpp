#include "client_ui.hpp"

#include "overlay.hpp"
#include "style.hpp"

#include <glad/gl.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <implot.h>

#include <algorithm>
#include <cstdio>
#include <exception>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <vector>

namespace sranipatch
{
  struct ClientUi::Impl
  {
    static constexpr SurfaceSize initial_size { 900, 834 };
    GLFWwindow* window {};
    std::unique_ptr<Overlay> overlay;
    bool glfw {}, imgui {}, implot {}, glfw_backend {}, opengl_backend {}, rendering {};
    std::vector<char> font_data;
    std::function<void()> draw;
    std::exception_ptr error;
    double last_frame_time {};

    ~Impl()
    {
      detach();
      if (window)
        glfwMakeContextCurrent(window);
      overlay.reset();
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

    void initialize(const std::filesystem::path& font, Presentation presentation)
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
      const bool in_overlay = presentation == Presentation::overlay;
      glfwWindowHint(GLFW_VISIBLE, in_overlay ? GLFW_FALSE : GLFW_TRUE);
      window = glfwCreateWindow(initial_size.width, initial_size.height, "SRanipatch", nullptr, nullptr);
      if (!window)
        throw std::runtime_error("OpenGL 3.3 window creation failed");
      glfwMakeContextCurrent(window);
      if (!gladLoadGL(glfwGetProcAddress) || !GLAD_GL_VERSION_3_3)
        throw std::runtime_error("OpenGL 3.3 loader initialization failed");
      glfwSwapInterval(in_overlay ? 0 : 1);
      if (in_overlay)
        overlay = std::make_unique<Overlay>(initial_size, font.parent_path() / "icon-128.png");
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
      if (!overlay)
      {
        glfw_backend = ImGui_ImplGlfw_InitForOpenGL(window, true);
        if (!glfw_backend)
          throw std::runtime_error("GUI window initialization failed");
      }
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
      if (overlay)
      {
        width = initial_size.width;
        height = initial_size.height;
      }
      else
        glfwGetFramebufferSize(window, &width, &height);
      if (width <= 0 || height <= 0)
        return;
      rendering = true;
      try
      {
        ImGui_ImplOpenGL3_NewFrame();
        if (overlay)
        {
          auto& io = ImGui::GetIO();
          io.DisplaySize = ImVec2(static_cast<float>(width), static_cast<float>(height));
          io.DisplayFramebufferScale = ImVec2(1, 1);
          const double now = glfwGetTime();
          io.DeltaTime = static_cast<float>(std::clamp(now - last_frame_time, 0.000001, 0.1));
          last_frame_time = now;
        }
        else
          ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        draw();
        ImGui::Render();
        if (overlay)
          overlay->bind_target();
        else
          glfwGetFramebufferSize(window, &width, &height);
        glViewport(0, 0, width, height);
        glClearColor(0.08F, 0.09F, 0.11F, 1.0F);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        if (overlay)
          overlay->submit();
        else
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
      last_frame_time = glfwGetTime();
      glfwSetWindowUserPointer(window, this);
      // GLFW fixes the order and types of the framebuffer dimensions.
      // NOLINTBEGIN(bugprone-easily-swappable-parameters)
      glfwSetFramebufferSizeCallback(
        window, [](GLFWwindow*, int width, int height) { glViewport(0, 0, width, height); }
      );
      // NOLINTEND(bugprone-easily-swappable-parameters)
      glfwSetWindowRefreshCallback(window, [](GLFWwindow* refreshed_window) {
        auto& self = *static_cast<Impl*>(glfwGetWindowUserPointer(refreshed_window));
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
          if (overlay && !overlay->poll())
            break;
          if (error)
            std::rethrow_exception(error);
          if (!interrupted && !glfwWindowShouldClose(window) && (!overlay || overlay->visible()))
            frame(false);
          if (overlay)
            overlay->wait_frame();
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

  ClientUi::ClientUi(const std::filesystem::path& font, Presentation presentation)
    : impl_(std::make_unique<Impl>())
  {
    impl_->initialize(font, presentation);
  }

  ClientUi::~ClientUi() = default;

  void ClientUi::run(const std::function<void()>& draw, const volatile std::sig_atomic_t& interrupted)
  {
    impl_->run(draw, interrupted);
  }
}  // namespace sranipatch
