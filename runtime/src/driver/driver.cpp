#include "recovery.hpp"
#include "sranipatch/process.hpp"
#include "sranipatch/protocol.hpp"
#include "sranipatch/runtime_control.hpp"

#include <openvr_driver.h>

#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace
{
  using Clock = std::chrono::steady_clock;
  constexpr auto section = "driver_sranipatch";
  constexpr auto heartbeat_timeout = 3'000'000'000ULL;
  constexpr auto shutdown_timeout = std::chrono::seconds(8);
  int module_anchor;

  enum class DisplayStatus : std::uint8_t
  {
    waiting,
    starting,
    running,
    manual,
    stopped,
    retrying,
    server_failed,
    overlay_failed,
    manual_offline,
    control_failed
  };

  std::filesystem::path runtime_directory()
  {
#ifdef _WIN32
    HMODULE module {};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
          reinterpret_cast<LPCWSTR>(&module_anchor), &module))
      throw std::runtime_error("Cannot locate SRanipatch driver module");
    std::array<wchar_t, 32768> path {};
    const auto length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size())
      throw std::runtime_error("Cannot resolve SRanipatch driver path");
    const auto module_path = std::filesystem::path(path.data());
#else
    Dl_info module {};
    if (!dladdr(&module_anchor, &module) || !module.dli_fname)
      throw std::runtime_error("Cannot locate SRanipatch driver module");
    const auto module_path = std::filesystem::absolute(module.dli_fname);
#endif
    // Driver binaries live in <package>/driver/bin/<platform>; applications in <package>/bin.
    return module_path.parent_path().parent_path().parent_path().parent_path() / "bin";
  }

  bool fresh(const sranipatch::ServerControl& server)
  {
    const auto now = sranipatch::monotonic_ns();
    return server.session_id && server.heartbeat_ns <= now && now - server.heartbeat_ns < heartbeat_timeout;
  }

  class Provider : public vr::IServerTrackedDeviceProvider
  {
  public:
    vr::EVRInitError Init(vr::IVRDriverContext* context) override
    {
      VR_INIT_SERVER_DRIVER_CONTEXT(context);
      try
      {
        // Resolve interfaces before starting the worker; OpenVR's lazy helpers are not synchronized.
        settings_ = vr::VRSettings();
        log_ = vr::VRDriverLog();
        worker_ = std::jthread([this](const std::stop_token& stop) { supervise(stop); });
      }
      catch (const std::exception& error)
      {
        try
        {
          status(std::string("SRanipatch driver failed: ") + error.what(), DisplayStatus::control_failed);
        }
        catch (...)
        {
          if (settings_)
          {
            settings_->SetString(section, "status", "SRanipatch driver failed to start");
            settings_->SetInt32(section, "statusState", static_cast<std::int32_t>(DisplayStatus::control_failed));
          }
        }
      }
      catch (...)
      {
        if (settings_)
        {
          settings_->SetString(section, "status", "SRanipatch driver failed to start");
          settings_->SetInt32(section, "statusState", static_cast<std::int32_t>(DisplayStatus::control_failed));
        }
      }
      // A tracking failure must never prevent SteamVR from starting.
      return vr::VRInitError_None;
    }

    void Cleanup() override
    {
      if (worker_.joinable())
      {
        worker_.request_stop();
        worker_.join();
      }
      VR_CLEANUP_SERVER_DRIVER_CONTEXT();
    }

    const char* const* GetInterfaceVersions() override { return vr::k_InterfaceVersions; }

    void RunFrame() override {}

    bool ShouldBlockStandbyMode() override { return false; }

    void EnterStandby() override {}

    void LeaveStandby() override {}

  private:
    bool command(const char* key)
    {
      if (!settings_->GetBool(section, key))
        return false;
      settings_->SetBool(section, key, false);
      return true;
    }

    void publish_status()
    {
      if (settings_ && settings_->GetInt32(section, "statusState") != static_cast<std::int32_t>(display_status_))
        settings_->SetInt32(section, "statusState", static_cast<std::int32_t>(display_status_));
    }

    void status(const std::string& message, DisplayStatus state)
    {
      display_status_ = state;
      publish_status();
      if (message == status_)
        return;
      status_ = message;
      if (settings_)
        settings_->SetString(section, "status", message.c_str());
      if (log_)
        log_->Log(("SRanipatch: " + message).c_str());
    }

    void supervise(const std::stop_token& stop)
    {
      try
      {
        run(stop);
      }
      catch (const std::exception& error)
      {
        try
        {
          status(std::string("Driver control failed; restart SteamVR: ") + error.what(), DisplayStatus::control_failed);
        }
        catch (...)
        {
          settings_->SetString(section, "status", "Driver control failed; restart SteamVR");
          settings_->SetInt32(section, "statusState", static_cast<std::int32_t>(DisplayStatus::control_failed));
        }
      }
      catch (...)
      {
        settings_->SetString(section, "status", "Driver control failed; restart SteamVR");
        settings_->SetInt32(section, "statusState", static_cast<std::int32_t>(DisplayStatus::control_failed));
      }
    }

    void run(const std::stop_token& stop)
    {
      std::array<char, 128> ipc {};
      settings_->GetString(section, "ipc", ipc.data(), static_cast<std::uint32_t>(ipc.size()));
      const std::string ipc_name = ipc[0] ? ipc.data() : "sranipatch";
      sranipatch::ControlChannel channel(ipc_name);
      sranipatch::DriverControl control;
      control.session_id = sranipatch::monotonic_ns();
      control.overlay_revision = 1;
      sranipatch::ChildProcess child;
      sranipatch::Recovery recovery;
      const auto directory = runtime_directory();
#ifdef _WIN32
      const auto executable = directory / "sranipatch-server.exe";
#else
      const auto executable = directory / "sranipatch-server";
#endif
      bool manual {};
      bool exhausted {};
      bool starting {};
      bool attached {};
      bool launched_once {};
      bool run_enabled = true;
      const auto initial_heartbeat = channel.read().server.heartbeat_ns;
      auto next_start = Clock::now() + std::chrono::milliseconds(500);
      Clock::time_point launched {};
      std::uint64_t server_session {};
      std::string overlay_error;
      // Action settings represent requests, never preferences carried over between SteamVR runs.
      settings_->SetBool(section, "retryServer", false);
      settings_->SetBool(section, "restartServer", false);
      settings_->SetBool(section, "serverManaged", true);

      const auto failed = [&](const std::string& reason) {
        child.stop(shutdown_timeout);
        starting = false;
        attached = false;
        if (recovery.failed(Clock::now()))
        {
          next_start = Clock::now() + std::chrono::seconds(1);
          status(reason + "; retrying once", DisplayStatus::retrying);
        }
        else
        {
          exhausted = true;
          status(reason + "; stopped after repeated failures. Use Retry server.", DisplayStatus::server_failed);
        }
      };

      while (!stop.stop_requested())
      {
        const auto now = Clock::now();
        const auto snapshot = channel.read();
        const bool available = fresh(snapshot.server);
        const bool retry = command("retryServer");
        const bool restart = command("restartServer");
        bool requested_run = settings_->GetBool(section, "runServer");

        if (!launched_once && !manual && available
          && snapshot.server.heartbeat_ns != initial_heartbeat)
        {
          manual = true;
          attached = true;
          server_session = snapshot.server.session_id;
          settings_->SetBool(section, "serverManaged", false);
          status("Connected to manually started server", DisplayStatus::manual);
        }

        if ((retry || restart) && manual)
          status("Manually started server: restart and retry are unavailable", DisplayStatus::manual);
        else if (retry || restart)
        {
          settings_->SetBool(section, "runServer", true);
          requested_run = true;
          child.stop(shutdown_timeout);
          recovery.reset();
          exhausted = false;
          starting = false;
          attached = false;
          next_start = Clock::now();
          status("Restarting server", DisplayStatus::starting);
        }

        if (!manual && !requested_run)
        {
          child.stop(shutdown_timeout);
          starting = false;
          attached = false;
        }
        else if (!manual && !run_enabled)
        {
          recovery.reset();
          exhausted = false;
          next_start = Clock::now();
        }
        run_enabled = requested_run;
        const bool desired_overlay = settings_->GetBool(section, "showOverlay") && (manual || run_enabled);
        if (control.show_overlay != desired_overlay)
        {
          control.show_overlay = desired_overlay;
          ++control.overlay_revision;
          if (desired_overlay)
            overlay_error.clear();
        }

        if (manual)
        {
          if (!available)
            status("Manually started server is offline; start it manually to reconnect", DisplayStatus::manual_offline);
          else if (server_session != snapshot.server.session_id || !attached)
          {
            server_session = snapshot.server.session_id;
            attached = true;
            overlay_error.clear();
            status("Connected to manually started server", DisplayStatus::manual);
          }
          attached = available;
        }
        else if (!run_enabled)
        {
          status("Tracking stopped", DisplayStatus::stopped);
        }
        else if (child.active())
        {
          if (const auto exit = child.poll())
          {
            failed("Server exited with code " + std::to_string(*exit));
          }
          else if (available && snapshot.server.session_id != server_session)
          {
            server_session = snapshot.server.session_id;
            starting = false;
            attached = true;
            overlay_error.clear();
            recovery.healthy(now);
            status("Tracking server running", DisplayStatus::running);
          }
          else if (attached && available)
          {
            recovery.healthy(now);
          }
          else if (attached || (starting && now - launched >= std::chrono::seconds(15)))
          {
            failed(starting ? "Server did not become ready" : "Server stopped responding");
          }
        }
        else if (!exhausted && now >= next_start)
        {
          try
          {
            launched_once = true;
            child.start(executable, { "--ipc", ipc_name }, directory);
            launched = now;
            starting = true;
            status("Starting tracking server", DisplayStatus::starting);
          }
          catch (const std::exception& error)
          {
            failed(std::string("Cannot start server: ") + error.what());
          }
        }

        if (attached && available && snapshot.server.session_id == server_session)
        {
          const bool overlay_acknowledged = snapshot.server.overlay_revision == control.overlay_revision;
          if (overlay_acknowledged && snapshot.server.overlay == sranipatch::OverlayState::failed)
            overlay_error = std::string("Overlay stopped: ") + snapshot.server.message.data();
          if (overlay_acknowledged && control.show_overlay
            && (snapshot.server.overlay == sranipatch::OverlayState::closed
              || snapshot.server.overlay == sranipatch::OverlayState::failed))
          {
            settings_->SetBool(section, "showOverlay", false);
            control.show_overlay = false;
            ++control.overlay_revision;
          }
          if (!overlay_error.empty())
            status(overlay_error, DisplayStatus::overlay_failed);
          else if (snapshot.server.message[0])
            status(snapshot.server.message.data(), DisplayStatus::retrying);
          else
            status(manual ? "Connected to manually started server" : "Tracking server running", manual ? DisplayStatus::manual : DisplayStatus::running);
        }
        control.heartbeat_ns = sranipatch::monotonic_ns();
        sranipatch::set_text(control.message, status_);
        channel.publish_driver(control);
        publish_status();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }

      // Detach manual servers; an owned server stops its client before exiting.
      control.heartbeat_ns = 0;
      control.show_overlay = false;
      channel.publish_driver(control);
      child.stop(shutdown_timeout);
      status("SteamVR stopped", DisplayStatus::stopped);
    }

    std::jthread worker_;
    vr::IVRSettings* settings_ {};
    vr::IVRDriverLog* log_ {};
    std::string status_;
    DisplayStatus display_status_ { DisplayStatus::waiting };
  };

  Provider provider;
}

#ifdef _WIN32
#define SRANIPATCH_DRIVER_EXPORT extern "C" __declspec(dllexport)
#else
#define SRANIPATCH_DRIVER_EXPORT extern "C" __attribute__((visibility("default")))
#endif

SRANIPATCH_DRIVER_EXPORT void* HmdDriverFactory(const char* interface_name, int* return_code)
{
  if (interface_name && std::strcmp(interface_name, vr::IServerTrackedDeviceProvider_Version) == 0)
    return &provider;
  if (return_code)
    *return_code = vr::VRInitError_Init_InterfaceNotFound;
  return nullptr;
}
