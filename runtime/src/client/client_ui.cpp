#include "client_ui.hpp"

#include "overlay.hpp"
#include "style.hpp"
#ifdef _WIN32
#include "windows_chrome.hpp"
#include "windows_surface.hpp"
#endif
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
#ifdef _WIN32
    GLFWwindow* context_window {};
    std::unique_ptr<WindowsSurface> windows_surface;
    ImFont* caption_font {};
#endif
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
      {
#ifdef _WIN32
        glfwMakeContextCurrent(context_window ? context_window : window);
        windows_surface.reset();
#else
        glfwMakeContextCurrent(window);
#endif
      }
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
#ifdef _WIN32
      if (context_window)
        glfwDestroyWindow(context_window);
#endif
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
      glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
#ifdef _WIN32
      if (!in_overlay)
      {
        context_window = glfwCreateWindow(1, 1, "SRanipatch renderer", nullptr, nullptr);
        if (!context_window)
          throw std::runtime_error("OpenGL 3.3 context creation failed");
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
      }
#endif
      window = glfwCreateWindow(initial_size.width, initial_size.height, "SRanipatch", nullptr, nullptr);
      if (!window)
        throw std::runtime_error("OpenGL 3.3 window creation failed");
#ifdef _WIN32
      if (!in_overlay)
        configure_windows_chrome(window);
      glfwMakeContextCurrent(context_window ? context_window : window);
#else
      glfwMakeContextCurrent(window);
#endif
      if (!gladLoadGL(glfwGetProcAddress) || !GLAD_GL_VERSION_3_3)
        throw std::runtime_error("OpenGL 3.3 loader initialization failed");
      glfwSwapInterval(in_overlay ? 0 : 1);
#ifdef _WIN32
      if (!in_overlay)
        windows_surface = std::make_unique<WindowsSurface>(window);
#endif
      if (in_overlay)
        overlay = std::make_unique<Overlay>(initial_size, font.parent_path() / "icon-128.png");
      IMGUI_CHECKVERSION();
      ImGui::CreateContext();
      imgui = true;
      ImPlot::CreateContext();
      implot = true;
      ImGui::GetIO().IniFilename = nullptr;
      set_dark_style();
      ImFontConfig config;
      config.FontDataOwnedByAtlas = false;
      std::snprintf(config.Name, sizeof(config.Name), "Quicksand Medium");
      if (!ImGui::GetIO().Fonts->AddFontFromMemoryTTF(font_data.data(), static_cast<int>(font_data.size()),
            18.0f, &config))
        throw std::runtime_error("Cannot load bundled Quicksand font");
#ifdef _WIN32
      if (windows_surface)
        caption_font = load_windows_caption_font();
#endif
      if (!overlay)
      {
#ifdef _WIN32
        glfw_backend = windows_surface ? ImGui_ImplGlfw_InitForOther(window, true)
                                       : ImGui_ImplGlfw_InitForOpenGL(window, true);
#else
        glfw_backend = ImGui_ImplGlfw_InitForOpenGL(window, true);
#endif
        if (!glfw_backend)
          throw std::runtime_error("GUI window initialization failed");
      }
      opengl_backend = ImGui_ImplOpenGL3_Init("#version 330");
      if (!opengl_backend)
        throw std::runtime_error("GUI renderer initialization failed");
      if (!in_overlay)
        glfwShowWindow(window);
    }

    void detach()
    {
#ifdef _WIN32
      if (windows_surface)
        windows_surface->stop();
#endif
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
#ifdef _WIN32
      else if (windows_surface)
        std::tie(width, height) = windows_surface->begin_frame();
#endif
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
        {
          ImGui_ImplGlfw_NewFrame();
#ifdef _WIN32
          if (windows_surface)
          {
            auto& io = ImGui::GetIO();
            io.DisplaySize = ImVec2(static_cast<float>(width), static_cast<float>(height));
            io.DisplayFramebufferScale = ImVec2(1, 1);
          }
#endif
        }
        ImGui::NewFrame();
#ifdef _WIN32
        if (!overlay)
        {
          auto* viewport = ImGui::GetMainViewport();
          const auto caption = windows_caption_height(window);
          viewport->WorkPos.y += caption;
          viewport->WorkSize.y = std::max(0.0f, viewport->WorkSize.y - caption);
        }
#endif
        draw();
#ifdef _WIN32
        if (!overlay)
          draw_windows_caption(window, caption_font);
#endif
        ImGui::Render();
        if (overlay)
          overlay->bind_target();
        else
#ifdef _WIN32
          if (!windows_surface)
#endif
          glfwGetFramebufferSize(window, &width, &height);
#ifdef _WIN32
        if (windows_surface)
          windows_surface->bind_target(width, height);
#endif
        glViewport(0, 0, width, height);
#ifdef _WIN32
        glClearColor(0.0F, 0.0F, 0.0F, 0.0F);
#else
        glClearColor(0.08F, 0.09F, 0.11F, 1.0F);
#endif
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        if (overlay)
          overlay->submit();
#ifdef _WIN32
        else if (windows_surface)
          windows_surface->present();
#endif
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
#ifdef _WIN32
        if (self.windows_surface)
          return;
#endif
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
#ifdef _WIN32
          if (windows_surface)
            windows_surface->poll();
          else
#endif
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
