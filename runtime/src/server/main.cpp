#include "sranipatch/capture.hpp"
#include "sranipatch/config.hpp"
#include "sranipatch/inference.hpp"
#include "sranipatch/ipc.hpp"
#include "sranipatch/osc.hpp"

#include <atomic>
#include <charconv>
#include <csignal>
#include <iostream>
#include <mutex>
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
    sranipatch::InferencePrecision precision { default_precision };
    std::string ipc { "sranipatch" };
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
    sranipatch::set_text(options.config.model, "../assets/models/lip_model.mnn");
    sranipatch::set_text(options.config.osc_host, "127.0.0.1");
    sranipatch::set_text(options.config.osc_address, "/sranipatch/parameters");
    const auto config_path = sranipatch::default_config_path();
    if (sranipatch::load_osc_config(config_path, options.config))
      std::cout << "Loaded configuration from " << config_path << '\n';
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
    std::cout << "sranipatch-server [options]\n" "  --model PATH      Lip MNN graph (default ../assets/models/lip_model.mnn)\n" "  --threads N       MNN CPU threads (default 2)\n" "  --precision MODE  MNN CPU precision: normal or low (default "
              << precision_name(default_precision)
              << ")\n" "  --osc-host IP --osc-port N --osc-address /path\n" "  --ipc NAME        Shared-memory channel (default sranipatch)\n";
  }

  int serve(const Options& options)
  {
    sranipatch::SharedMemory ipc(options.ipc, sranipatch::SharedMemory::Role::server);
    sranipatch::Status status;
    status.config = options.config;
    status.session_id = sranipatch::monotonic_ns();
    status.heartbeat_ns = status.session_id;
    ipc.publish(status);
    try
    {
      auto inference = sranipatch::open_lip_mnn(options.config, options.precision);
      std::cout << "MNN CPU precision: " << precision_name(options.precision) << '\n';
      sranipatch::OscSender osc(options.config.osc_host.data(),
        static_cast<std::uint16_t>(options.config.osc_port),
        options.config.osc_address.data());
      sranipatch::LatestFrame latest;
      std::atomic<std::uint64_t> captured {};
      std::atomic<bool> capture_connected {};
      std::atomic<bool> capture_fault {};
      std::mutex error_mutex;
      std::string capture_error;
      std::jthread capture([&](const std::stop_token& stop) {
        try
        {
          while (!stop.stop_requested())
          {
            try
            {
              auto camera = sranipatch::open_capture();
              capture_connected.store(true, std::memory_order_release);
              std::cout << "Camera connected\n";
              sranipatch::Frame frame;
              auto last_frame_time = sranipatch::monotonic_ns();
              while (!stop.stop_requested())
              {
                if (camera->read(frame, stop))
                {
                  last_frame_time = sranipatch::monotonic_ns();
                  captured.fetch_add(1, std::memory_order_relaxed);
                  latest.push(frame);
                }
                if (sranipatch::monotonic_ns() - last_frame_time > 5'000'000'000ULL)
                  throw sranipatch::CaptureUnavailable(
                    "Camera has not delivered a valid frame for five seconds"
                  );
              }
            }
            catch (const sranipatch::CaptureUnavailable& error)
            {
              capture_connected.store(false, std::memory_order_release);
              std::cerr << "Camera unavailable: " << error.what() << "; waiting for USB device\n";
              sranipatch::wait_for_capture_device(stop, error.new_arrival_required());
              if (!stop.stop_requested())
                std::cout << "VIVE Facial Tracker detected; reconnecting\n";
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
      sranipatch::Frame frame;
      std::array<float, sranipatch::max_parameters> values {};
      std::uint64_t last_sequence {}, previous_captured {}, previous_inferred {};
      auto rate_time = sranipatch::monotonic_ns();
      std::uint64_t publish_time {};
      std::uint64_t preview_time {};
      while (!interrupted)
      {
        if (capture_fault.load(std::memory_order_acquire))
        {
          std::lock_guard lock(error_mutex);
          throw std::runtime_error(capture_error);
        }
        if (latest.pop(frame, capture.get_stop_token()))
        {
          if (frame.sequence > last_sequence + 1)
            status.frames_skipped += frame.sequence - last_sequence - 1;
          last_sequence = frame.sequence;
          const auto begin = sranipatch::monotonic_ns();
          if (begin - preview_time >= 33'000'000ULL)
          {
            if (ipc.publish_frame(frame))
              preview_time = begin;
          }
          const auto inference_begin = sranipatch::monotonic_ns();
          status.parameter_count = inference->run(frame, status.parameters);
          const auto end = sranipatch::monotonic_ns();
          ++status.frames_inferred;
          status.inference_ms = static_cast<float>(end - inference_begin) / 1.0e6F;
          status.frame_age_ms = static_cast<float>(end - frame.timestamp_ns) / 1.0e6F;
          for (std::size_t i = 0; i < status.parameter_count; ++i)
            values[i] = status.parameters[i].value;
          if (!osc.send(std::span(values.data(), status.parameter_count)))
            ++status.osc_failures;
        }
        const auto now = sranipatch::monotonic_ns();
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
        if (!capture_connected.load(std::memory_order_acquire))
          status.state = sranipatch::ServerState::waiting_for_camera;
        else
          status.state = sranipatch::ServerState::running;
        if (now - publish_time >= 33'000'000ULL)
        {
          status.heartbeat_ns = now;
          if (ipc.publish(status))
            publish_time = now;
        }
      }
      capture.request_stop();
      capture.join();
      status.state = sranipatch::ServerState::stopped;
    }
    catch (const std::exception& error)
    {
      status.state = sranipatch::ServerState::fault;
      sranipatch::set_text(status.error, error.what());
      std::cerr << "Server fault: " << error.what() << '\n';
    }
    status.heartbeat_ns = sranipatch::monotonic_ns();
    for (int i = 0; i < 10 && !ipc.publish(status); ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return status.state == sranipatch::ServerState::fault ? 1 : 0;
  }
}  // namespace

// Exceptions from the body are caught below; clang-tidy misidentifies MSVC exception inheritance.
// NOLINTNEXTLINE(bugprone-exception-escape)
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
