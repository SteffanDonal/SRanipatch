#include "tracking.hpp"

#include "sranipatch/capture.hpp"
#include "sranipatch/live_osc_config.hpp"

#include <atomic>
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace
{
  using namespace std::chrono_literals;
  std::atomic<unsigned> camera_opens {}, camera_closes {}, model_loads {}, model_runs {};
  std::mutex camera_mutex;

  std::condition_variable_any& camera_ready()
  {
    static std::condition_variable_any ready;
    return ready;
  }

  unsigned pending_frames {}, produced_frames {}, acknowledged_frames {};
  std::atomic<bool> camera_unavailable {};
  std::atomic<unsigned> device_waits {};
#ifndef _WIN32
  std::string discovered_peer;
#endif
  void require(bool condition, const char* message)
  {
    if (!condition)
      throw std::runtime_error(message);
  }

  class Camera final : public sranipatch::Capture
  {
    std::thread::id owner_ { std::this_thread::get_id() };

  public:
    ~Camera() override
    {
      if (owner_ != std::this_thread::get_id())
        std::terminate();
      ++camera_closes;
    }

    bool read(sranipatch::Frame& frame, std::stop_token stop) override
    {
      std::unique_lock lock(camera_mutex);
      acknowledged_frames = produced_frames;
      camera_ready().notify_all();
      if (!camera_ready().wait_for(lock, stop, 100ms, [] { return pending_frames != 0; }))
        return false;
      --pending_frames;
      ++produced_frames;
      frame.sequence = 1;  // Device sequence restarts whenever it is opened.
      frame.timestamp_ns = sranipatch::monotonic_ns();
      frame.width = frame.height = 1;
      frame.luma = { 123 };
      return true;
    }
  };

  class Model final : public sranipatch::Inference
  {
  public:
    std::uint32_t run(const sranipatch::Frame&,
      std::array<sranipatch::Parameter, sranipatch::max_parameters>& out,
      std::span<float>) override
    {
      ++model_runs;
      out[0].value = 0.75F;
      return 1;
    }
  };

  void idle_startup_uses_no_camera_or_model()
  {
    sranipatch::Tracking tracking({}, sranipatch::InferencePrecision::normal);
    sranipatch::Status status;
    sranipatch::Frame frame;
    require(!tracking.update(false, status, frame), "Idle must not produce tracking");
    require(camera_opens == 0 && model_loads == 0 && model_runs == 0,
      "Idle startup opened hardware or loaded inference");
    require(status.state == sranipatch::ServerState::waiting_for_steam_link,
      "Idle must be reported distinctly from missing camera hardware");
  }

  void peer_lifecycle_retains_model_and_discards_tracking()
  {
    sranipatch::Tracking tracking({}, sranipatch::InferencePrecision::normal);
    sranipatch::Status status;
    sranipatch::Frame frame;
    auto deliver = [&] {
      {
        std::lock_guard lock(camera_mutex);
        ++pending_frames;
      }
      camera_ready().notify_all();
      const auto deadline = std::chrono::steady_clock::now() + 2s;
      while (std::chrono::steady_clock::now() < deadline)
      {
        if (tracking.update(true, status, frame))
          return;
      }
      throw std::runtime_error("Active tracking did not process a camera frame");
    };
    deliver();
    const auto first_sequence = frame.sequence;
    require(camera_opens == 1 && model_loads == 1 && status.frames_inferred == 1 && status.parameters[0].value == 0.75F,
      "Active session must start capture and inference");
    status.capture_fps = status.inference_fps = 60;
    tracking.update(false, status, frame);
    require(camera_closes == 1 && status.parameter_count == 0 && status.parameters[0].value == 0 && frame.luma.empty() && status.capture_fps == 0 && status.inference_fps == 0 && status.inference_ms == 0 && status.frame_age_ms == 0,
      "Idle must close camera and clear tracking, preview, and current rates");
    tracking.update(false, status, frame);
    require(model_runs == 1 && camera_opens == 1, "Idle must do no capture or inference work");
    deliver();
    require(camera_opens == 2 && model_loads == 1 && model_runs == 2 && frame.sequence > first_sequence && status.frames_inferred == 2 && status.frames_captured == 2,
      "Reconnect must retain model and advance frame IDs and cumulative counters");
  }

  void queued_frames_are_discarded_on_idle()
  {
    const auto runs = model_runs.load();
    sranipatch::Tracking tracking({}, sranipatch::InferencePrecision::normal);
    sranipatch::Status status;
    sranipatch::Frame frame;
    tracking.update(true, status, frame);
    {
      std::unique_lock lock(camera_mutex);
      const auto before = produced_frames;
      ++pending_frames;
      camera_ready().notify_all();
      require(camera_ready().wait_for(lock, 2s, [&] { return acknowledged_frames > before; }),
        "Camera did not finish queuing a frame");
    }
    tracking.update(false, status, frame);
    require(status.frames_captured == 1 && model_runs == runs,
      "Test camera did not queue an uninferred frame");
    require(!tracking.update(true, status, frame) && model_runs == runs,
      "Reconnect processed a frame queued before idle");
  }

  void idle_cancels_waiting_for_camera()
  {
    sranipatch::Tracking tracking({}, sranipatch::InferencePrecision::normal);
    sranipatch::Status status;
    sranipatch::Frame frame;
    camera_unavailable = true;
    const auto waits = device_waits.load();
    tracking.update(true, status, frame);
    {
      std::unique_lock lock(camera_mutex);
      require(camera_ready().wait_for(lock, 2s, [&] { return device_waits > waits; }),
        "Capture did not enter device arrival wait");
    }
    require(status.state == sranipatch::ServerState::waiting_for_camera && device_waits > waits,
      "Missing camera must wait for device arrival");
    const auto begin = std::chrono::steady_clock::now();
    tracking.update(false, status, frame);
    require(std::chrono::steady_clock::now() - begin < 1s && status.state == sranipatch::ServerState::waiting_for_steam_link,
      "Entering idle did not cancel device arrival wait");
    camera_unavailable = false;
  }

#ifndef _WIN32
  void live_config_switches_idle_policy()
  {
    struct File
    {
      std::filesystem::path path { std::filesystem::temp_directory_path() / ("sranipatch-idle-" + std::to_string(sranipatch::monotonic_ns())) };

      ~File()
      {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
      }

      void write(std::string_view text) const
      {
        std::ofstream(path) << text;
      }
    } file;

    sranipatch::Config defaults;
    sranipatch::LiveOscConfig live(file.path, defaults, {});
    sranipatch::Tracking tracking(defaults, sranipatch::InferencePrecision::normal);
    sranipatch::Status status;
    sranipatch::Frame frame;
    const auto opens = camera_opens.load();
    const auto loads = model_loads.load();
    auto await_opens = [&](unsigned count) {
      std::unique_lock lock(camera_mutex);
      require(camera_ready().wait_for(lock, 2s, [&] { return camera_opens == count; }),
        "Capture thread did not open camera");
    };
    tracking.update(!live.sender().waiting_for_peer(), status, frame);
    require(camera_opens == opens && model_loads == loads, "Automatic mode must start idle without peer");
    auto now = std::chrono::steady_clock::now();
    file.write("osc_host=127.0.0.1\n");
    live.poll(now);
    require(live.poll(now + 300ms), "Explicit host config was not applied");
    tracking.update(!live.sender().waiting_for_peer(), status, frame);
    await_opens(opens + 1);
    require(camera_opens == opens + 1 && model_loads == loads + 1,
      "Explicit IP must activate tracking without Steam Link");
    now += 1s;
    file.write("");
    live.poll(now);
    require(live.poll(now + 300ms), "Automatic mode config was not applied");
    tracking.update(!live.sender().waiting_for_peer(), status, frame);
    require(status.state == sranipatch::ServerState::waiting_for_steam_link,
      "Switch to automatic mode without a peer must idle immediately");
    discovered_peer = "127.0.0.1";
    std::this_thread::sleep_for(2100ms);
    live.sender().refresh();
    tracking.update(!live.sender().waiting_for_peer(), status, frame);
    await_opens(opens + 2);
    require(camera_opens == opens + 2 && model_loads == loads + 1,
      "Discovered session must restart capture and reuse model");
    discovered_peer.clear();
  }
#endif
}  // namespace

namespace sranipatch
{
#ifndef _WIN32
  std::string steam_link_peer(const std::filesystem::path&)
  {
    return discovered_peer;
  }
#endif
  std::unique_ptr<Capture> open_capture()
  {
    {
      std::lock_guard lock(camera_mutex);
      ++camera_opens;
    }
    camera_ready().notify_all();
    if (camera_unavailable)
      throw CaptureUnavailable("Test camera unavailable");
    return std::make_unique<Camera>();
  }

  void wait_for_capture_device(std::stop_token stop, bool)
  {
    std::unique_lock lock(camera_mutex);
    ++device_waits;
    camera_ready().notify_all();
    camera_ready().wait_for(lock, std::move(stop), 5s, [] { return !camera_unavailable; });
  }

  std::unique_ptr<Inference> open_lip_mnn(const Config&, InferencePrecision)
  {
    ++model_loads;
    return std::make_unique<Model>();
  }
}  // namespace sranipatch

// NOLINTNEXTLINE(bugprone-exception-escape): Exceptions from the body are caught below.
int main()
{
  try
  {
    idle_startup_uses_no_camera_or_model();
    peer_lifecycle_retains_model_and_discards_tracking();
    queued_frames_are_discarded_on_idle();
    idle_cancels_waiting_for_camera();
#ifndef _WIN32
    live_config_switches_idle_policy();
#endif
    std::cout << "Tracking lifecycle checks passed\n";
    return 0;
  }
  catch (const std::exception& error)
  {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
