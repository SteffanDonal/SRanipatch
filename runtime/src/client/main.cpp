#include "head_preview.hpp"
#include "parameter_grid.hpp"
#include "performance_plot.hpp"
#include "sranipatch/ipc.hpp"
#include "style.hpp"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <implot.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

namespace
{
  volatile std::sig_atomic_t interrupted {};

  void interrupt(int)
  {
    interrupted = 1;
  }

  const char* state_name(sranipatch::ServerState state)
  {
    switch (state)
    {
      case sranipatch::ServerState::starting:
        return "Starting";
      case sranipatch::ServerState::waiting_for_camera:
        return "Waiting for camera";
      case sranipatch::ServerState::running:
        return "Running";
      case sranipatch::ServerState::stopped:
        return "Stopped";
      case sranipatch::ServerState::fault:
        return "Fault";
    }
    return "Unknown";
  }

  void glfw_error(int, const char* message)
  {
    std::cerr << "GLFW: " << message << '\n';
  }

  std::filesystem::path default_asset_path(const char* filename)
  {
#ifdef _WIN32
    std::array<wchar_t, 32768> executable {};
    const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (length && length < executable.size())
      return std::filesystem::path(executable.data()).parent_path() / filename;
#else
    std::error_code error;
    const auto executable = std::filesystem::read_symlink("/proc/self/exe", error);
    if (!error)
      return executable.parent_path() / filename;
#endif
    return filename;
  }

  struct Options
  {
    std::string ipc { "sranipatch" };
    std::filesystem::path head { default_asset_path("head.glb") };
    bool help {};
  };

  Options options(int argc, char** argv)
  {
    Options result;
    for (int i = 1; i < argc; ++i)
    {
      const std::string_view key(argv[i]);
      if (key == "--help")
      {
        result.help = true;
        continue;
      }
      if (++i == argc)
        throw std::runtime_error("Missing client option value");
      if (key == "--ipc")
        result.ipc = argv[i];
      else if (key == "--head")
        result.head = argv[i];
      else
        throw std::runtime_error("Unknown client option: " + std::string(key));
    }
    return result;
  }

  static void PushStyleCompact()
  {
    ImGuiStyle& style = ImGui::GetStyle();
    auto frame_padding = style.FramePadding.y * 0.6f;
    auto cell_padding = style.CellPadding.y * 0.6f;
    auto item_spacing = style.ItemSpacing.y * 0.6f;

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2 { frame_padding, frame_padding });
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2 { item_spacing, item_spacing });
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2 { cell_padding, cell_padding });
    ImGui::PushFont(nullptr, 15.0f);
  }

  static void PopStyleCompact()
  {
    ImGui::PopStyleVar(3);
    ImGui::PopFont();
  }

  int run(const Options& options)
  {
    const auto font_path = default_asset_path("Quicksand-Medium.ttf");
    std::ifstream font_file(font_path, std::ios::binary);
    std::vector<char> font_data(std::istreambuf_iterator<char> { font_file }, {});
    if (font_file.bad() || font_data.empty())
      throw std::runtime_error("Cannot read bundled font: " + font_path.string());
    const auto& name = options.ipc;
    glfwSetErrorCallback(glfw_error);
    if (!glfwInit())
      throw std::runtime_error("GLFW initialization failed");
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    GLFWwindow* window = glfwCreateWindow(900, 720, "SRanipatch", nullptr, nullptr);
    if (!window)
    {
      glfwTerminate();
      throw std::runtime_error("OpenGL 3.3 window creation failed");
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    sranipatch::set_dark_pastel_style();
    ImFontConfig font_config;
    font_config.FontDataOwnedByAtlas = false;
    std::snprintf(font_config.Name, sizeof(font_config.Name), "Quicksand Medium");
    if (!ImGui::GetIO().Fonts->AddFontFromMemoryTTF(font_data.data(), static_cast<int>(font_data.size()),
          18.0f, &font_config))
    {
      ImPlot::DestroyContext();
      ImGui::DestroyContext();
      glfwDestroyWindow(window);
      glfwTerminate();
      throw std::runtime_error("Cannot load bundled Quicksand font");
    }
    if (!ImGui_ImplGlfw_InitForOpenGL(window, true) || !ImGui_ImplOpenGL3_Init("#version 330"))
    {
      ImPlot::DestroyContext();
      ImGui::DestroyContext();
      glfwDestroyWindow(window);
      glfwTerminate();
      throw std::runtime_error("GUI renderer initialization failed");
    }
    std::unique_ptr<sranipatch::SharedMemory> ipc;
    sranipatch::Status status;
    std::string connection_message { "Waiting for server" };
    std::uint64_t retry_at {}, session {};
    sranipatch::PerformancePlot performance;
    sranipatch::Frame preview;
    GLuint preview_texture {};
    glGenTextures(1, &preview_texture);
    glBindTexture(GL_TEXTURE_2D, preview_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    // Windows' OpenGL 1.1 headers omit this OpenGL 3.3 constant.
    constexpr GLint clamp_to_edge = 0x812F;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, clamp_to_edge);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, clamp_to_edge);
    constexpr GLenum texture_swizzle_green = 0x8E43;
    constexpr GLenum texture_swizzle_blue = 0x8E44;
    glTexParameteri(GL_TEXTURE_2D, texture_swizzle_green, GL_RED);
    glTexParameteri(GL_TEXTURE_2D, texture_swizzle_blue, GL_RED);
    std::uint32_t texture_width {}, texture_height {};
    int camera_view { 1 };
    std::unique_ptr<sranipatch::HeadPreview> head;
    std::string head_error;
    try
    {
      head = std::make_unique<sranipatch::HeadPreview>(options.head);
    }
    catch (const std::exception& error)
    {
      head_error = error.what();
    }
    while (!interrupted && !glfwWindowShouldClose(window))
    {
      glfwPollEvents();
      const auto now = sranipatch::monotonic_ns();
      if (!ipc && now >= retry_at)
      {
        retry_at = now + 1'000'000'000ULL;
        try
        {
          ipc =
            std::make_unique<sranipatch::SharedMemory>(name, sranipatch::SharedMemory::Role::client);
        }
        catch (const std::exception& error)
        {
          connection_message = error.what();
        }
      }
      bool fresh = false;
      if (ipc)
      {
        try
        {
          ipc->read(status);
          const auto observed_at = sranipatch::monotonic_ns();
          fresh = status.heartbeat_ns && observed_at >= status.heartbeat_ns && observed_at - status.heartbeat_ns < 2'000'000'000ULL;
          if (session != status.session_id)
          {
            session = status.session_id;
            preview = {};
          }
          if (ipc->read_frame(preview))
          {
            glBindTexture(GL_TEXTURE_2D, preview_texture);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            if (texture_width != preview.width || texture_height != preview.height)
            {
              texture_width = preview.width;
              texture_height = preview.height;
              constexpr GLint red8 = 0x8229;
              glTexImage2D(GL_TEXTURE_2D, 0, red8, static_cast<GLsizei>(preview.width),
                static_cast<GLsizei>(preview.height), 0, GL_RED, GL_UNSIGNED_BYTE,
                preview.luma.data());
            }
            else
              glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, static_cast<GLsizei>(preview.width),
                static_cast<GLsizei>(preview.height), GL_RED, GL_UNSIGNED_BYTE,
                preview.luma.data());
          }
          // Windows mappings disappear when all handles close; reopen after a server exits.
          if (!fresh || status.state == sranipatch::ServerState::fault || status.state == sranipatch::ServerState::stopped)
          {
            connection_message = "Server is offline or its heartbeat is stale";
            ipc.reset();
          }
        }
        catch (const std::exception& error)
        {
          connection_message = error.what();
          ipc.reset();
        }
      }
      performance.update(status, fresh && static_cast<bool>(ipc), now);
      ImGui_ImplOpenGL3_NewFrame();
      ImGui_ImplGlfw_NewFrame();
      ImGui::NewFrame();
      ImGui::SetNextWindowPos(ImVec2(0, 0));
      ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
      ImGui::Begin("Server", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
      if (ImGui::BeginTable("dashboard", 2, ImGuiTableFlags_Resizable))
      {
        ImGui::TableNextColumn();

        if (ImGui::BeginTable("status", 2))
        {
          ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed);
          ImGui::TableSetupColumn("control", ImGuiTableColumnFlags_WidthStretch);

          ImGui::TableNextColumn();
          ImGui::Text("Status");
          ImGui::TableNextColumn();
          ImGui::TextWrapped("%s%s", state_name(status.state), fresh ? "" : " (offline)");

          if (!ipc)
          {
            ImGui::TableNextColumn();
            ImGui::TableNextColumn();
            ImGui::TextWrapped("%s", connection_message.c_str());
          }
          else
          {
            if (status.error[0])
            {
              ImGui::TableNextColumn();
              ImGui::TableNextColumn();
              ImGui::TextWrapped("%s", status.error.data());
            }

            ImGui::TableNextColumn();
            ImGui::Text("Threads");
            ImGui::TableNextColumn();
            ImGui::Text("%u", status.config.threads);

            ImGui::TableNextColumn();
            ImGui::Text("OSC");
            ImGui::TableNextColumn();
            if (status.osc_failures > 0)
              ImGui::TextWrapped("%s:%u  Errors: %llu\n%s", status.config.osc_host.data(),
                status.config.osc_port,
                static_cast<unsigned long long>(status.osc_failures),
                status.config.osc_address.data());
            else
              ImGui::TextWrapped("%s:%u\n%s", status.config.osc_host.data(), status.config.osc_port,
                status.config.osc_address.data());
          }

          ImGui::EndTable();
        }

        PushStyleCompact();
        performance.draw(status);
        PopStyleCompact();

        ImGui::TableNextColumn();
        if (ImGui::BeginTabBar("preview"))
        {
          if (ImGui::BeginTabItem("Blendshapes"))
          {
            if (head)
            {
              const float side = std::clamp(ImGui::GetContentRegionAvail().x, 64.0F, 400.0F);
              const auto count =
                fresh ? std::min<std::size_t>(status.parameter_count, sranipatch::max_parameters)
                      : 0;
              try
              {
                const int render_width = std::clamp(
                  static_cast<int>(side * ImGui::GetIO().DisplayFramebufferScale.x), 64, 1024
                );
                const auto texture = head->render(std::span(status.parameters.data(), count),
                  render_width, render_width / 2);
                ImGui::Image(static_cast<ImTextureID>(texture), ImVec2(side, side * 0.5F),
                  ImVec2(1, 1), ImVec2(0, 0));
              }
              catch (const std::exception& error)
              {
                head_error = error.what();
                head.reset();
              }
            }
            if (!head_error.empty())
              ImGui::TextWrapped("Head preview: %s", head_error.c_str());
            ImGui::EndTabItem();
          }
          if (ImGui::BeginTabItem("Camera"))
          {
            ImGui::Combo("View", &camera_view, "Both\0Left\0Right\0");
            if (!preview.luma.empty())
            {
              const float side = std::min(400.0F, ImGui::GetContentRegionAvail().x);
              const float left = camera_view == 2 ? 0.5F + 0.5F / 400.0F : 0.0F;
              const float right = camera_view == 1 ? 0.5F - 0.5F / 400.0F : 1.0F;
              const float height =
                side * static_cast<float>(preview.height) / static_cast<float>(preview.width);
              ImGui::Image(static_cast<ImTextureID>(preview_texture), ImVec2(side, height),
                ImVec2(left, 0), ImVec2(right, 1));
            }
            else
              ImGui::TextUnformatted("Waiting for camera frames...");
            ImGui::EndTabItem();
          }
          ImGui::EndTabBar();
        }

        ImGui::EndTable();
      }

      ImGui::SeparatorText("Parameters");
      PushStyleCompact();
      if (ImGui::BeginChild("parameters", ImGui::GetContentRegionAvail(), ImGuiChildFlags_Borders))
      {
        const auto count = std::min<std::size_t>(status.parameter_count, sranipatch::max_parameters);
        sranipatch::draw_parameter_grid(std::span(status.parameters.data(), count));
      }
      PopStyleCompact();
      ImGui::EndChild();

      ImGui::End();
      ImGui::Render();
      int width {}, height {};
      glfwGetFramebufferSize(window, &width, &height);
      glViewport(0, 0, width, height);
      glClearColor(0.08F, 0.09F, 0.11F, 1.0F);
      glClear(GL_COLOR_BUFFER_BIT);
      ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
      glfwSwapBuffers(window);
    }
    head.reset();
    glDeleteTextures(1, &preview_texture);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
  }
}  // namespace

// Exceptions from the body are caught below; clang-tidy misidentifies MSVC exception inheritance.
// NOLINTNEXTLINE(bugprone-exception-escape)
int main(int argc, char** argv)
{
  try
  {
    std::signal(SIGINT, interrupt);
    std::signal(SIGTERM, interrupt);
    const auto parsed = options(argc, argv);
    if (parsed.help)
    {
      std::cout << "sranipatch-client [--ipc NAME] [--head PATH]\n";
      return 0;
    }
    return run(parsed);
  }
  catch (const std::exception& error)
  {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
