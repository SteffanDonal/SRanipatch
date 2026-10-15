#include "sof/ipc.hpp"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <algorithm>
#include <array>
#include <csignal>
#include <cstdio>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace
{
  volatile std::sig_atomic_t interrupted {};

  void interrupt(int)
  {
    interrupted = 1;
  }

  const char* state_name(sof::ServerState state)
  {
    switch (state)
    {
      case sof::ServerState::starting:
        return "Starting";
      case sof::ServerState::running:
        return "Running";
      case sof::ServerState::paused:
        return "Paused";
      case sof::ServerState::stopped:
        return "Stopped";
      case sof::ServerState::fault:
        return "Fault";
    }
    return "Unknown";
  }

  void glfw_error(int, const char* message)
  {
    std::cerr << "GLFW: " << message << '\n';
  }

  std::string channel(int argc, char** argv)
  {
    if (argc == 1)
      return "sranipal";
    if (argc == 2 && std::string_view(argv[1]) == "--help")
    {
      std::cout << "sof-client [--ipc NAME]\n";
      return {};
    }
    if (argc == 3 && std::string_view(argv[1]) == "--ipc")
      return argv[2];
    throw std::runtime_error("Usage: sof-client [--ipc NAME]");
  }

  int run(const std::string& name)
  {
    glfwSetErrorCallback(glfw_error);
    if (!glfwInit())
      throw std::runtime_error("GLFW initialization failed");
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    GLFWwindow* window = glfwCreateWindow(900, 720, "SRanipal on Frame", nullptr, nullptr);
    if (!window)
    {
      glfwTerminate();
      throw std::runtime_error("OpenGL 3.3 window creation failed");
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::StyleColorsDark();
    if (!ImGui_ImplGlfw_InitForOpenGL(window, true) || !ImGui_ImplOpenGL3_Init("#version 330"))
    {
      ImGui::DestroyContext();
      glfwDestroyWindow(window);
      glfwTerminate();
      throw std::runtime_error("GUI renderer initialization failed");
    }
    std::unique_ptr<sof::SharedMemory> ipc;
    sof::Status status;
    sof::Controls controls;
    std::string connection_message { "Waiting for server" };
    std::uint64_t retry_at {}, session {}, requested_revision {};
    bool paused {}, osc_enabled { true }, request_pending {};
    std::array<float, 120> rates {};
    std::size_t graph_index {};
    std::uint64_t graph_at {};
    sof::Frame preview;
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
    int camera_view {};
    while (!interrupted && !glfwWindowShouldClose(window))
    {
      glfwWaitEventsTimeout(1.0 / 30.0);
      const auto now = sof::monotonic_ns();
      if (!ipc && now >= retry_at)
      {
        retry_at = now + 1'000'000'000ULL;
        try
        {
          ipc = std::make_unique<sof::SharedMemory>(name, sof::SharedMemory::Role::client);
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
          ipc->read(status, controls);
          const auto observed_at = sof::monotonic_ns();
          fresh = status.heartbeat_ns && observed_at >= status.heartbeat_ns && observed_at - status.heartbeat_ns < 2'000'000'000ULL;
          if (session != status.session_id)
          {
            session = status.session_id;
            request_pending = false;
            requested_revision = 0;
            preview = {};
          }
          if (!request_pending && controls.revision <= status.applied_revision)
          {
            paused = status.state == sof::ServerState::paused;
            osc_enabled = status.osc_enabled != 0;
          }
          if (request_pending && requested_revision != 0 && status.applied_revision >= requested_revision)
            request_pending = false;
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
          if (!fresh || status.state == sof::ServerState::fault || status.state == sof::ServerState::stopped)
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
      if (now - graph_at > 200'000'000ULL)
      {
        rates[graph_index] = fresh ? status.inference_fps : 0.0F;
        graph_index = (graph_index + 1) % rates.size();
        graph_at = now;
      }
      ImGui_ImplOpenGL3_NewFrame();
      ImGui_ImplGlfw_NewFrame();
      ImGui::NewFrame();
      ImGui::SetNextWindowPos(ImVec2(0, 0));
      ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
      ImGui::Begin("Server", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
      if (ImGui::Button("Quit") || ImGui::IsKeyPressed(ImGuiKey_Escape))
        glfwSetWindowShouldClose(window, GLFW_TRUE);
      ImGui::SameLine();
      ImGui::Text("Channel: %s", name.c_str());
      ImGui::Text("Status: %s%s", state_name(status.state), fresh ? "" : " (offline)");
      if (!ipc)
        ImGui::TextWrapped("%s", connection_message.c_str());
      if (status.error[0])
        ImGui::TextWrapped("%s", status.error.data());
      if (ImGui::BeginTable("dashboard", 2, ImGuiTableFlags_Resizable))
      {
        ImGui::TableNextColumn();
        ImGui::SeparatorText("Camera preview");
        ImGui::Combo("View", &camera_view, "Stereo (packed)\0Left camera\0Right camera\0");
        if (!preview.luma.empty())
        {
          const float side = std::min(400.0F, ImGui::GetContentRegionAvail().x);
          const float left = camera_view == 2 ? 0.5F : 0.0F;
          const float right = camera_view == 1 ? 0.5F : 1.0F;
          const float height =
            side * static_cast<float>(preview.height) / static_cast<float>(preview.width);
          ImGui::Image(static_cast<ImTextureID>(preview_texture), ImVec2(side, height), ImVec2(left, 0),
            ImVec2(right, 1));
          ImGui::Text("Frame %llu", static_cast<unsigned long long>(preview.sequence));
        }
        else
          ImGui::TextUnformatted("Waiting for camera frames...");
        ImGui::TableNextColumn();
        ImGui::SeparatorText("Performance");
        ImGui::Text("Capture: %.1f fps    Inference: %.1f fps", status.capture_fps, status.inference_fps);
        ImGui::Text("Inference: %.2f ms    Frame age: %.2f ms", status.inference_ms, status.frame_age_ms);
        ImGui::Text("Captured: %llu    Inferred: %llu\nSkipped: %llu    OSC failures: %llu",
          static_cast<unsigned long long>(status.frames_captured),
          static_cast<unsigned long long>(status.frames_inferred),
          static_cast<unsigned long long>(status.frames_skipped),
          static_cast<unsigned long long>(status.osc_failures));
        ImGui::PlotLines("Inference FPS", rates.data(), static_cast<int>(rates.size()),
          static_cast<int>(graph_index), nullptr, 0.0F, 120.0F, ImVec2(0, 80));
        ImGui::SeparatorText("Controls");
        ImGui::BeginDisabled(!ipc || !fresh);
        bool changed =
          ImGui::Checkbox(status.inference_enabled ? "Pause inference" : "Pause preview", &paused);
        ImGui::BeginDisabled(!status.inference_enabled);
        changed |= ImGui::Checkbox("Send OSC", &osc_enabled);
        ImGui::EndDisabled();
        if (changed)
          request_pending = true;
        if (ipc && fresh && request_pending && (changed || requested_revision == 0))
        {
          if (!ipc->control(paused, osc_enabled, requested_revision))
            requested_revision = 0;
        }
        ImGui::EndDisabled();
        if (request_pending)
          ImGui::TextUnformatted("Waiting for server to apply controls...");
        ImGui::SeparatorText("Configuration");
        ImGui::TextWrapped("Model: %s",
          status.inference_enabled ? status.config.model.data() : "Capture only");
        ImGui::Text("VIVE Facial Tracker: %ux%u @ %u fps    MNN threads: %u", sof::capture_width,
          sof::capture_height, sof::capture_fps, status.config.threads);
        ImGui::TextWrapped("OSC: %s:%u %s", status.config.osc_host.data(), status.config.osc_port,
          status.config.osc_address.data());
        ImGui::TextUnformatted("Lip input: stereo grayscale, 2 x 100 x 100, scaled to [0, 1]");
        ImGui::TextUnformatted("Lip output: 37 ordered SRanipal v2 shapes");
        ImGui::TextUnformatted("Model settings are supplied to the server at startup.");
        ImGui::EndTable();
      }
      ImGui::SeparatorText("Parameters");
      if (ImGui::BeginTable("parameters", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
            ImVec2(0, 0)))
      {
        ImGui::TableSetupColumn("Name");
        ImGui::TableSetupColumn("Value");
        ImGui::TableHeadersRow();
        for (std::size_t i = 0; i < status.parameter_count; ++i)
        {
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(status.parameters[i].name.data());
          ImGui::TableNextColumn();
          ImGui::Text("%.6f", status.parameters[i].value);
        }
        ImGui::EndTable();
      }
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
    glDeleteTextures(1, &preview_texture);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
  }
}  // namespace

int main(int argc, char** argv)
{
  try
  {
    std::signal(SIGINT, interrupt);
    std::signal(SIGTERM, interrupt);
    const auto name = channel(argc, argv);
    return name.empty() ? 0 : run(name);
  }
  catch (const std::exception& error)
  {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
