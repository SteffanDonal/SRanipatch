#include "runtime.hpp"
#include "sranipatch/config.hpp"
#include "sranipatch/diagnostics.hpp"
#include "sranipatch/inference.hpp"
#include "sranipatch/ipc.hpp"
#include "sranipatch/live_osc_config.hpp"
#include "sranipatch/osc.hpp"
#include "sranipatch/process.hpp"
#include "tracking.hpp"

#include <charconv>
#include <csignal>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace
{
  constexpr auto default_precision = sranipatch::InferencePrecision::normal;

  constexpr std::string_view precision_name(sranipatch::InferencePrecision precision)
  {
    return precision == sranipatch::InferencePrecision::low ? "low" : "normal";
  }

  volatile std::sig_atomic_t interrupted = 0;

  void interrupt(int)
  {
    interrupted = 1;
  }

  struct Options
  {
    sranipatch::Config config;
    sranipatch::OscOverrides osc_overrides;
    sranipatch::InferencePrecision precision { default_precision };
    std::string ipc { "sranipatch" };
    std::string lifetime;
    bool help {};
  };

  template <std::size_t N>
  void argument_text(std::array<char, N>& dest, std::string_view value)
  {
    if (value.empty() || value.size() >= N)
      throw std::runtime_error("Argument string is empty or too long");
    sranipatch::set_text(dest, value);
  }

  std::uint32_t number(std::string_view value, std::uint32_t minimum, std::uint32_t maximum)
  {
    std::uint32_t result {};
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc {} || end != value.data() + value.size() || result < minimum || result > maximum)
      throw std::runtime_error("Numeric argument is invalid or outside the allowed range");
    return result;
  }

  Options parse(int argc, char** argv)
  {
    Options options;
    const auto model = (sranipatch::executable_directory() / "models/lip_model.mnn").u8string();
    argument_text(options.config.model, std::string(model.begin(), model.end()));
    for (int i = 1; i < argc; ++i)
    {
      const std::string_view key(argv[i]);
      if (key == "--help" || key == "-h")
      {
        options.help = true;
        continue;
      }
      if (++i >= argc)
        throw std::runtime_error("Missing value for " + std::string(key));
      const std::string_view value(argv[i]);
      if (key == "--model")
        argument_text(options.config.model, value);
      else if (key == "--ipc")
        options.ipc = value;
      else if (key == "--lifetime")
        options.lifetime = value;
      else if (key == "--threads")
        options.config.threads = number(value, 1, 64);
      else if (key == "--precision")
      {
        if (value == "normal")
          options.precision = sranipatch::InferencePrecision::normal;
        else if (value == "low")
          options.precision = sranipatch::InferencePrecision::low;
        else
          throw std::runtime_error("Precision must be normal or low");
      }
      else if (key == "--osc-host")
      {
        argument_text(options.config.osc_host, value);
        options.osc_overrides.host = true;
      }
      else if (key == "--osc-port")
      {
        options.config.osc_port = number(value, 1, 65535);
        options.osc_overrides.port = true;
      }
      else
        throw std::runtime_error("Unknown option: " + std::string(key));
    }
    return options;
  }

  void usage()
  {
    std::cout << "sranipatch-server [options]\n" "  --model PATH      Lip MNN graph (default models/lip_model.mnn)\n" "  --threads N       MNN CPU threads (default 2)\n" "  --precision MODE  MNN CPU precision: normal or low (default "
              << precision_name(default_precision)
              << ")\n" "  --osc-host IP --osc-port N\n" "  --ipc NAME        Shared-memory channel (default sranipatch)\n";
  }

  int serve(const Options& options)
  {
    sranipatch::ProcessLifetime lifetime;
    if (!options.lifetime.empty())
      lifetime = sranipatch::ProcessLifetime(options.lifetime);
    const auto config_path = sranipatch::default_config_path();
    sranipatch::LiveOscConfig live_config(config_path, options.config, options.osc_overrides);
    sranipatch::SharedMemory ipc(options.ipc, sranipatch::SharedMemory::Role::server);
    sranipatch::ServerRuntime runtime(options.ipc, !options.lifetime.empty());
    sranipatch::Status status;
    status.config = live_config.config();
    status.session_id = sranipatch::monotonic_ns();
    status.heartbeat_ns = status.session_id;
    sranipatch::log_event("server.session", "session=", status.session_id, " ipc=", options.ipc,
      " managed=", !options.lifetime.empty(), " precision=", precision_name(options.precision),
      " threads=", options.config.threads);
    ipc.publish(status);
    try
    {
      sranipatch::Tracking tracking(options.config, options.precision);
      std::cout << "Server running; IPC=" << options.ipc << '\n';
      sranipatch::Frame frame;
      std::array<float, sranipatch::max_parameters> values {};
      std::uint64_t previous_captured {}, previous_inferred {};
      auto rate_time = sranipatch::monotonic_ns();
      std::uint64_t publish_time {};
      std::uint64_t preview_time {};
      std::uint64_t summary_time {};
      auto previous_state = status.state;
      while (!interrupted && !lifetime.stop_requested())
      {
        try
        {
          if (live_config.poll())
          {
            status.config = live_config.config();
            std::cout << "Reloaded configuration from " << config_path << '\n';
          }
        }
        catch (const std::exception& error)
        {
          std::cerr << "Configuration reload failed (" << config_path << "): " << error.what()
                    << "; keeping the last working configuration\n";
        }
        auto& osc = live_config.sender();
        osc.refresh();
        const bool active = !osc.waiting_for_peer();
        if (tracking.update(active, status, frame))
        {
          const auto begin = sranipatch::monotonic_ns();
          if (begin - preview_time >= 33'000'000ULL)
          {
            if (ipc.publish_frame(frame))
              preview_time = begin;
          }
          for (std::size_t i = 0; i < status.parameter_count; ++i)
            values[i] = status.parameters[i].value;
          if (!osc.send(std::span(values.data(), status.parameter_count)))
            ++status.osc_failures;
        }
        const auto now = sranipatch::monotonic_ns();
        if (status.state != previous_state)
        {
          sranipatch::log_event("server.state", "session=", status.session_id, " from=",
            static_cast<int>(previous_state), " to=", static_cast<int>(status.state), " capture_active=", active);
          previous_state = status.state;
        }
        if (now - summary_time >= 5'000'000'000ULL)
        {
          sranipatch::log_summary("server.summary", "session=", status.session_id, " state=",
            static_cast<int>(status.state), " captured=", status.frames_captured, " inferred=",
            status.frames_inferred, " skipped=", status.frames_skipped, " capture_fps=", status.capture_fps,
            " inference_fps=", status.inference_fps, " inference_ms=", status.inference_ms,
            " frame_age_ms=", status.frame_age_ms, " osc_failures=", status.osc_failures);
          summary_time = now;
        }
        status.osc_destination = osc.destination();
        if (!active)
        {
          previous_captured = status.frames_captured;
          previous_inferred = status.frames_inferred;
          rate_time = now;
        }
        if (now - rate_time >= 1'000'000'000ULL)
        {
          const float seconds = static_cast<float>(now - rate_time) / 1.0e9F;
          status.capture_fps = static_cast<float>(status.frames_captured - previous_captured) / seconds;
          status.inference_fps =
            static_cast<float>(status.frames_inferred - previous_inferred) / seconds;
          previous_captured = status.frames_captured;
          previous_inferred = status.frames_inferred;
          rate_time = now;
          std::cout << "Capture: " << status.capture_fps << " fps, inference: " << status.inference_fps
                    << " fps (" << status.inference_ms << " ms), " << status.frames_captured
                    << " frames\n";
        }
        if (now - publish_time >= 33'000'000ULL)
        {
          status.heartbeat_ns = now;
          if (ipc.publish(status))
            publish_time = now;
        }
        if (!active)
          std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
      status.state = sranipatch::ServerState::stopped;
      sranipatch::log_event("server.stop_requested", "session=", status.session_id,
        " signal=", interrupted, " lifetime=", lifetime.stop_requested());
    }
    catch (const std::exception& error)
    {
      status.state = sranipatch::ServerState::fault;
      sranipatch::set_text(status.error, error.what());
      std::cerr << "Server fault: " << error.what() << '\n';
      sranipatch::log_event("server.fault", "session=", status.session_id, " error=", error.what());
    }
    status.heartbeat_ns = sranipatch::monotonic_ns();
    for (int i = 0; i < 10 && !ipc.publish(status); ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const int exit = status.state == sranipatch::ServerState::fault ? 1 : 0;
    sranipatch::log_event("server.exit", "session=", status.session_id, " code=", exit);
    return exit;
  }
}  // namespace

// Exceptions from the body are caught below; clang-tidy misidentifies MSVC exception inheritance.
// NOLINTNEXTLINE(bugprone-exception-escape)
int main(int argc, char** argv)
{
  sranipatch::DiagnosticSession diagnostics("server");
  sranipatch::cleanup_old_packages();
  std::cout << std::unitbuf;
  try
  {
    const auto options = parse(argc, argv);
    if (options.help)
    {
      usage();
      return 0;
    }
    std::signal(SIGINT, interrupt);
    std::signal(SIGTERM, interrupt);
    return serve(options);
  }
  catch (const std::exception& error)
  {
    sranipatch::log_event("server.start_failed", "error=", error.what());
    std::cerr << error.what() << '\n';
    return 1;
  }
}
