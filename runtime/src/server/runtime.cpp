#include "runtime.hpp"

#include "sranipatch/diagnostics.hpp"
#include "sranipatch/process.hpp"
#include "sranipatch/protocol.hpp"
#include "sranipatch/runtime_control.hpp"

#include <chrono>
#include <iostream>
#include <memory>
#include <utility>

namespace sranipatch
{
  namespace
  {
    constexpr std::uint64_t driver_timeout = 3'000'000'000ULL;
    constexpr std::uint64_t healthy_interval = 60'000'000'000ULL;
    constexpr std::uint64_t retry_delay = 1'000'000'000ULL;

    void supervise(const std::stop_token& stop, const std::string& ipc, bool managed,
      const std::filesystem::path& executable)
    {
      std::unique_ptr<ControlChannel> channel;
      ChildProcess client;
      ServerControl status;
      status.session_id = monotonic_ns();
      status.managed = managed;
      std::uint64_t driver_session {}, retry_command {}, started {}, retry_at {};
      unsigned failures {};
      bool enabled {}, finished {}, reported_error {};
      while (!stop.stop_requested())
      {
        try
        {
          if (!channel)
            channel = std::make_unique<ControlChannel>(ipc);
          const auto control = channel->read();
          const auto now = monotonic_ns();
          const bool connected = control.driver.session_id != 0
            && control.driver.heartbeat_ns <= now
            && now - control.driver.heartbeat_ns < driver_timeout;
          const bool requested = connected && control.driver.show_overlay;
          if (control.driver.session_id != driver_session
            || control.driver.retry_client != retry_command
            || control.driver.overlay_revision != status.overlay_revision || (requested && !enabled))
          {
            driver_session = control.driver.session_id;
            retry_command = control.driver.retry_client;
            failures = 0;
            finished = false;
            retry_at = 0;
            status.message.fill('\0');
          }
          enabled = requested;
          if (!enabled)
          {
            if (client.active())
              log_event("overlay.stop_requested", "driver_connected=", connected,
                " driver_session=", control.driver.session_id, " requested=", control.driver.show_overlay);
            client.stop();
            status.overlay = OverlayState::stopped;
            finished = false;
          }
          else
          {
            if (client.active())
            {
              const auto exit = client.poll();
              if (exit)
              {
                log_event("overlay.child_exit", "code=", *exit, " failures=", failures,
                  " server_control_session=", status.session_id);
                if (*exit == 2)
                {
                  status.overlay = OverlayState::closed;
                  finished = true;
                }
                else if (*exit == 0)
                {
                  status.overlay = OverlayState::stopped;
                  finished = true;
                }
                else
                {
                  ++failures;
                  retry_at = now + retry_delay;
                  finished = failures >= 2;
                  status.overlay = finished ? OverlayState::failed : OverlayState::starting;
                  set_text(status.message, finished ? "Dashboard failed twice; enable it again to retry" : "Dashboard exited unexpectedly; retrying once");
                }
              }
              else if (now - started >= healthy_interval)
                failures = 0;
            }
            if (!client.active() && !finished && now >= retry_at)
            {
              try
              {
                log_event("overlay.launch.begin", "ipc=", ipc, " failures=", failures,
                  " driver_session=", driver_session, " server_control_session=", status.session_id);
                client.start(executable, { "--overlay", "--ipc", ipc }, executable.parent_path());
                log_event("overlay.launch.end");
                started = now;
                status.overlay = OverlayState::running;
                status.message.fill('\0');
              }
              catch (const std::exception& error)
              {
                log_event("overlay.launch_failed", "error=", error.what());
                ++failures;
                retry_at = now + retry_delay;
                finished = failures >= 2;
                status.overlay = finished ? OverlayState::failed : OverlayState::starting;
                set_text(status.message, error.what());
              }
            }
          }
          status.heartbeat_ns = now;
          status.overlay_revision = control.driver.overlay_revision;
          channel->publish_server(status);
          reported_error = false;
        }
        catch (const std::exception& error)
        {
          client.stop();
          channel.reset();
          if (!reported_error)
          {
            log_event("runtime.control_failed", "error=", error.what());
            std::cerr << "Runtime control unavailable: " << error.what() << '\n';
          }
          reported_error = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      client.stop();
      log_event("runtime.stop", "server_control_session=", status.session_id);
      if (channel)
      {
        try
        {
          status.heartbeat_ns = 0;
          status.overlay = OverlayState::stopped;
          channel->publish_server(status);
        }
        catch (const std::exception& error)
        {
          std::cerr << "Runtime control shutdown: " << error.what() << '\n';
        }
      }
    }
  }

  ServerRuntime::ServerRuntime(std::string ipc, bool managed, std::filesystem::path client)
  {
    if (client.empty())
    {
#ifdef _WIN32
      client = executable_directory() / "sranipatch-client.exe";
#else
      client = executable_directory() / "sranipatch-client";
#endif
    }
    worker_ = std::jthread([ipc = std::move(ipc), managed, client = std::move(client)](const std::stop_token& stop) {
      supervise(stop, ipc, managed, client);
    });
  }
}
