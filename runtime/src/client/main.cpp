#include "client_ui.hpp"
#include "head_preview.hpp"
#include "observation.hpp"
#include "parameter_grid.hpp"
#include "performance_plot.hpp"

#include <glad/gl.h>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
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
    sranipatch::Presentation presentation { sranipatch::Presentation::desktop };
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
      if (key == "--overlay")
      {
        result.presentation = sranipatch::Presentation::overlay;
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

  class CameraTexture
  {
  public:
    CameraTexture()
    {
      glGenTextures(1, &texture_);
      glBindTexture(GL_TEXTURE_2D, texture_);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, GL_RED);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
    }

    ~CameraTexture()
    {
      glDeleteTextures(1, &texture_);
    }

    CameraTexture(const CameraTexture&) = delete;
    CameraTexture& operator=(const CameraTexture&) = delete;

    void upload(const sranipatch::Frame& frame)
    {
      glBindTexture(GL_TEXTURE_2D, texture_);
      glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
      if (width_ != frame.width || height_ != frame.height)
      {
        width_ = frame.width;
        height_ = frame.height;
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, static_cast<GLsizei>(width_), static_cast<GLsizei>(height_),
          0, GL_RED, GL_UNSIGNED_BYTE, frame.luma.data());
      }
      else
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, static_cast<GLsizei>(width_),
          static_cast<GLsizei>(height_), GL_RED, GL_UNSIGNED_BYTE, frame.luma.data());
    }

    GLuint id() const
    {
      return texture_;
    }

  private:
    GLuint texture_ {};
    std::uint32_t width_ {}, height_ {};
  };

  void stale_overlay(const char* label)
  {
    const auto min = ImGui::GetItemRectMin();
    const auto max = ImGui::GetItemRectMax();
    auto* draw = ImGui::GetWindowDrawList();
    const auto size = ImGui::CalcTextSize(label);
    draw->PushClipRect(min, max, true);
    draw->AddRectFilled(min, ImVec2(min.x + size.x + 12, min.y + size.y + 8), IM_COL32(20, 23, 28, 220), 3);
    draw->AddText(ImVec2(min.x + 6, min.y + 4), IM_COL32(220, 220, 220, 255), label);
    draw->PopClipRect();
  }

  int run(const Options& options)
  {
    sranipatch::ClientUi ui(default_asset_path("Quicksand-Medium.ttf"), options.presentation);
    sranipatch::Observation observation(options.ipc);
    sranipatch::PerformancePlot performance;
    CameraTexture camera_texture;
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

    ui.run(
      [&] {
        const auto& observed = observation.update();
        const auto& status = observed.status;
        const auto& preview = observed.frame;
        if (observed.frame_changed)
          camera_texture.upload(preview);
        performance.update(observed, sranipatch::monotonic_ns());
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
            ImGui::TextWrapped("%s%s", state_name(status.state),
              observed.available ? "" : " (offline)");

            if (!observed.available)
            {
              ImGui::TableNextColumn();
              ImGui::TableNextColumn();
              ImGui::TextWrapped("%s", observed.message.empty() ? "Waiting for server" : observed.message.c_str());
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
              const auto& destination = status.osc_destination;
              if (destination.host[0])
              {
                const bool ipv6 =
                  std::string_view(destination.host.data()).find(':') != std::string_view::npos;
                ImGui::TextWrapped("%s%s%s:%u%s", ipv6 ? "[" : "", destination.host.data(),
                  ipv6 ? "]" : "", destination.port,
                  destination.ready ? "" : " (last)");
              }
              else
              {
                ImGui::TextUnformatted("Waiting for Steam Link");
              }
              if (status.osc_failures > 0)
                ImGui::Text("Errors: %llu", static_cast<unsigned long long>(status.osc_failures));
              ImGui::TextWrapped("%s", status.config.osc_address.data());
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
                const float side = std::max(ImGui::GetContentRegionAvail().x, 64.0F);
                const auto count = observed.tracking_fresh
                  ? std::min<std::size_t>(status.parameter_count,
                      sranipatch::max_parameters)
                  : 0;
                try
                {
                  const int render_width = std::clamp(
                    static_cast<int>(side * ImGui::GetIO().DisplayFramebufferScale.x), 64,
                    1024
                  );
                  const auto texture = head->render(std::span(status.parameters.data(), count),
                    render_width, render_width / 2);
                  ImGui::Image(static_cast<ImTextureID>(texture), ImVec2(side, side * 0.5F),
                    ImVec2(1, 1), ImVec2(0, 0));
                  if (!observed.tracking_fresh)
                    stale_overlay("Tracking stale");
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
                const float tint = observed.camera_fresh ? 1.0F : 0.45F;
                ImGui::ImageWithBg(static_cast<ImTextureID>(camera_texture.id()),
                  ImVec2(side, height), ImVec2(left, 0), ImVec2(right, 1),
                  ImVec4(0, 0, 0, 0), ImVec4(tint, tint, tint, 1));
                if (!observed.camera_fresh)
                  stale_overlay("Camera stale");
              }
              else
                ImGui::TextUnformatted("Waiting for camera frames...");
              ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
          }

          ImGui::EndTable();
        }

        ImGui::SeparatorText(observed.tracking_fresh ? "Parameters" : "Parameters (stale)");
        PushStyleCompact();
        if (ImGui::BeginChild("parameters", ImGui::GetContentRegionAvail(), ImGuiChildFlags_Borders))
        {
          const auto count = std::min<std::size_t>(status.parameter_count, sranipatch::max_parameters);
          ImGui::BeginDisabled(!observed.tracking_fresh);
          sranipatch::draw_parameter_grid(std::span(status.parameters.data(), count));
          ImGui::EndDisabled();
        }
        PopStyleCompact();
        ImGui::EndChild();

        ImGui::End();
      },
      interrupted
    );
    return 0;
  }
}  // namespace

// Exceptions from the body are caught below; clang-tidy misidentifies MSVC exception inheritance.
// NOLINTNEXTLINE(bugprone-exception-escape)
int client_main(int argc, char** argv)
{
  try
  {
    std::signal(SIGINT, interrupt);
    std::signal(SIGTERM, interrupt);
    const auto parsed = options(argc, argv);
    if (parsed.help)
    {
      std::cout << "sranipatch-client [--ipc NAME] [--head PATH] [--overlay]\n";
      return 0;
    }
    return run(parsed);
  }
  catch (const std::exception& error)
  {
#ifdef _WIN32
    MessageBoxA(nullptr, error.what(), "SRanipatch client", MB_OK | MB_ICONERROR);
#else
    std::cerr << error.what() << '\n';
#endif
    return 1;
  }
}

#ifdef _WIN32
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
  return client_main(__argc, __argv);
}
#else
int main(int argc, char** argv)
{
  return client_main(argc, argv);
}
#endif
