#include "sof/capture.hpp"
#include "sof/config.hpp"
#include "sof/inference.hpp"
#include "sof/ipc.hpp"
#include "sof/osc.hpp"

#include <atomic>
#include <charconv>
#include <cmath>
#include <csignal>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace
{
#if defined(__aarch64__) || defined(_M_ARM64)
  constexpr auto default_precision = sof::InferencePrecision::low;
#else
  constexpr auto default_precision = sof::InferencePrecision::normal;
#endif

  constexpr std::string_view precision_name(sof::InferencePrecision precision)
  {
    return precision == sof::InferencePrecision::low ? "low" : "normal";
  }

  volatile std::sig_atomic_t interrupted = 0;

  void interrupt(int)
  {
    interrupted = 1;
  }

  struct Options
  {
    sof::Config config;
    sof::InferencePrecision precision { default_precision };
    std::string ipc { "sranipal" };
    bool help {};
    bool capture_only {};
    std::uint32_t seconds {};
  };

  template <std::size_t N>
  void argument_text(std::array<char, N>& dest, std::string_view value)
  {
    if (value.empty() || value.size() >= N)
      throw std::runtime_error("Argument string is empty or too long");
    sof::set_text(dest, value);
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
    sof::set_text(options.config.model, "../assets/models/lip_model.mnn");
    sof::set_text(options.config.osc_host, "127.0.0.1");
    sof::set_text(options.config.osc_address, "/sranipal/parameters");
    const auto config_path = sof::default_config_path();
    if (sof::load_osc_config(config_path, options.config))
      std::cout << "Loaded configuration from " << config_path << '\n';
    for (int i = 1; i < argc; ++i)
    {
      const std::string_view key(argv[i]);
      if (key == "--help" || key == "-h")
      {
        options.help = true;
        continue;
      }
      if (key == "--capture-only")
      {
        options.capture_only = true;
        continue;
      }
      if (++i >= argc)
        throw std::runtime_error("Missing value for " + std::string(key));
      const std::string_view value(argv[i]);
      if (key == "--model")
        argument_text(options.config.model, value);
      else if (key == "--ipc")
        options.ipc = value;
      else if (key == "--seconds")
        options.seconds = number(value, 1, 86400);
      else if (key == "--threads")
        options.config.threads = number(value, 1, 64);
      else if (key == "--precision")
      {
        if (value == "normal")
          options.precision = sof::InferencePrecision::normal;
        else if (value == "low")
          options.precision = sof::InferencePrecision::low;
        else
          throw std::runtime_error("Precision must be normal or low");
      }
      else if (key == "--osc-host")
        argument_text(options.config.osc_host, value);
      else if (key == "--osc-port")
        options.config.osc_port = number(value, 1, 65535);
      else if (key == "--osc-address")
        argument_text(options.config.osc_address, value);
      else
        throw std::runtime_error("Unknown option: " + std::string(key));
    }
    return options;
  }

  void usage()
  {
    std::cout << "sof-server [options]\n" "  --capture-only    Capture and preview without inference or OSC\n" "  --model PATH      Lip MNN graph (default ../assets/models/lip_model.mnn)\n" "  --seconds N       Stop after N seconds (optional)\n" "  --threads N       MNN CPU threads (default 2)\n" "  --precision MODE  MNN CPU precision: normal or low (default "
              << precision_name(default_precision)
              << ")\n" "  --osc-host IP --osc-port N --osc-address /path\n" "  --ipc NAME        Shared-memory channel (default sranipal)\n";
  }

  int serve(const Options& options)
  {
    sof::SharedMemory ipc(options.ipc, sof::SharedMemory::Role::server);
    sof::Status status;
    sof::Controls controls;
    status.config = options.config;
    status.inference_enabled = options.capture_only ? 0U : 1U;
    status.osc_enabled = options.capture_only ? 0U : 1U;
    status.session_id = sof::monotonic_ns();
    status.heartbeat_ns = status.session_id;
    ipc.publish(status, controls);
    try
    {
      std::unique_ptr<sof::Inference> inference;
      std::unique_ptr<sof::OscSender> osc;
      if (!options.capture_only)
      {
        inference = sof::open_lip_mnn(options.config, options.precision);
        std::cout << "MNN CPU precision: " << precision_name(options.precision) << '\n';
        osc = std::make_unique<sof::OscSender>(options.config.osc_host.data(),
          static_cast<std::uint16_t>(options.config.osc_port),
          options.config.osc_address.data());
      }
      sof::LatestFrame latest;
      std::atomic<std::uint64_t> captured {};
      std::atomic<bool> capture_fault {};
      std::mutex error_mutex;
      std::string capture_error;
      std::jthread capture([&](std::stop_token stop) {
        try
        {
          auto camera = sof::open_capture();
          sof::Frame frame;
          while (!stop.stop_requested())
          {
            if (camera->read(frame, stop))
            {
              captured.fetch_add(1, std::memory_order_relaxed);
              latest.push(frame);
            }
          }
        }
        catch (const std::exception& error)
        {
          {
            std::lock_guard lock(error_mutex);
            capture_error = error.what();
          }
          capture_fault.store(true, std::memory_order_release);
        }
      });
      std::cout << "Server running; IPC=" << options.ipc << '\n';
      sof::Frame frame;
      std::array<float, sof::max_parameters> values {};
      std::uint64_t last_sequence {}, previous_captured {}, previous_inferred {};
      auto rate_time = sof::monotonic_ns();
      auto last_frame_time = rate_time;
      std::uint64_t publish_time {};
      std::uint64_t preview_time {};
      const auto stop_at =
        options.seconds ? rate_time + static_cast<std::uint64_t>(options.seconds) * 1'000'000'000ULL : 0;
      while (!interrupted && (!stop_at || sof::monotonic_ns() < stop_at))
      {
        if (capture_fault.load(std::memory_order_acquire))
        {
          std::lock_guard lock(error_mutex);
          throw std::runtime_error(capture_error);
        }
        if (latest.pop(frame, capture.get_stop_token()))
        {
          last_frame_time = sof::monotonic_ns();
          if (frame.sequence > last_sequence + 1)
            status.frames_skipped += frame.sequence - last_sequence - 1;
          last_sequence = frame.sequence;
          if (!controls.paused)
          {
            const auto begin = sof::monotonic_ns();
            if (begin - preview_time >= 33'000'000ULL)
            {
              if (ipc.publish_frame(frame))
                preview_time = begin;
            }
            if (inference)
            {
              const auto inference_begin = sof::monotonic_ns();
              status.parameter_count = inference->run(frame, status.parameters);
              const auto end = sof::monotonic_ns();
              ++status.frames_inferred;
              status.inference_ms = static_cast<float>(end - inference_begin) / 1.0e6F;
              status.frame_age_ms = static_cast<float>(end - frame.timestamp_ns) / 1.0e6F;
              if (controls.osc_enabled)
              {
                for (std::size_t i = 0; i < status.parameter_count; ++i)
                  values[i] = status.parameters[i].value;
                if (!osc->send(std::span(values.data(), status.parameter_count)))
                  ++status.osc_failures;
              }
            }
            else
              status.frame_age_ms =
                static_cast<float>(sof::monotonic_ns() - frame.timestamp_ns) / 1.0e6F;
          }
          else
            ++status.frames_skipped;
        }
        const auto now = sof::monotonic_ns();
        if (now - last_frame_time > 5'000'000'000ULL)
          throw std::runtime_error("Camera has not delivered a frame for five seconds");
        status.frames_captured = captured.load(std::memory_order_relaxed);
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
        status.state = controls.paused ? sof::ServerState::paused : sof::ServerState::running;
        status.osc_enabled = inference ? controls.osc_enabled : 0U;
        status.applied_revision = controls.revision;
        if (now - publish_time >= 33'000'000ULL)
        {
          status.heartbeat_ns = now;
          if (ipc.publish(status, controls))
            publish_time = now;
        }
      }
      capture.request_stop();
      capture.join();
      status.state = sof::ServerState::stopped;
    }
    catch (const std::exception& error)
    {
      status.state = sof::ServerState::fault;
      sof::set_text(status.error, error.what());
      std::cerr << "Server fault: " << error.what() << '\n';
    }
    status.heartbeat_ns = sof::monotonic_ns();
    for (int i = 0; i < 10 && !ipc.publish(status, controls); ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return status.state == sof::ServerState::fault ? 1 : 0;
  }
}  // namespace

int main(int argc, char** argv)
{
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
    std::cerr << error.what() << '\n';
    return 1;
  }
}
