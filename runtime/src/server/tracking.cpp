#include "tracking.hpp"

#include "sranipatch/capture.hpp"

#include <iostream>
#include <mutex>
#include <thread>

namespace sranipatch
{
  struct Tracking::CaptureRun
  {
    LatestFrame latest;
    std::atomic<bool> connected {};
    std::atomic<bool> fault {};
    std::mutex error_mutex;
    std::string error;
    std::jthread thread;

    explicit CaptureRun(std::atomic<std::uint64_t>& captured)
      : thread([this, &captured](const std::stop_token& stop) {
          try
          {
            while (!stop.stop_requested())
            {
              try
              {
                auto camera = open_capture();
                connected.store(true, std::memory_order_release);
                std::cout << "Camera connected\n";
                Frame frame;
                auto last_frame_time = monotonic_ns();
                while (!stop.stop_requested())
                {
                  if (camera->read(frame, stop) && !stop.stop_requested())
                  {
                    last_frame_time = monotonic_ns();
                    frame.sequence = captured.fetch_add(1, std::memory_order_relaxed) + 1;
                    latest.push(frame);
                  }
                  if (monotonic_ns() - last_frame_time > 5'000'000'000ULL)
                    throw CaptureUnavailable(
                      "Camera has not delivered a valid frame for five seconds"
                    );
                }
              }
              catch (const CaptureUnavailable& failure)
              {
                connected.store(false, std::memory_order_release);
                if (!stop.stop_requested())
                {
                  std::cerr << "Camera unavailable: " << failure.what()
                            << "; waiting for USB device\n";
                  wait_for_capture_device(stop, failure.new_arrival_required());
                }
              }
            }
          }
          catch (const std::exception& failure)
          {
            {
              std::lock_guard lock(error_mutex);
              error = failure.what();
            }
            fault.store(true, std::memory_order_release);
          }
        }) {}
  };

  Tracking::Tracking(Config config, InferencePrecision precision) : config_(config), precision_(precision) {}

  Tracking::~Tracking() = default;

  bool Tracking::update(bool active, Status& status, Frame& frame)
  {
    if (!active)
    {
      capture_.reset();
      last_sequence_ = captured_.load(std::memory_order_relaxed);
      status.frames_captured = last_sequence_;
      frame = {};
      status.parameters = {};
      status.parameter_count = 0;
      status.capture_fps = status.inference_fps = 0;
      status.inference_ms = status.frame_age_ms = 0;
      status.state = ServerState::waiting_for_steam_link;
      return false;
    }
    if (!inference_)
      inference_ = open_lip_mnn(config_, precision_);
    if (!capture_)
      capture_ = std::make_unique<CaptureRun>(captured_);
    if (capture_->fault.load(std::memory_order_acquire))
    {
      std::lock_guard lock(capture_->error_mutex);
      throw std::runtime_error(capture_->error);
    }
    const bool received = capture_->latest.pop(frame, capture_->thread.get_stop_token());
    status.frames_captured = captured_.load(std::memory_order_relaxed);
    status.state = capture_->connected.load(std::memory_order_acquire) ? ServerState::running
                                                                       : ServerState::waiting_for_camera;
    if (!received)
      return false;
    if (frame.sequence > last_sequence_ + 1)
      status.frames_skipped += frame.sequence - last_sequence_ - 1;
    last_sequence_ = frame.sequence;
    const auto begin = monotonic_ns();
    status.parameter_count = inference_->run(frame, status.parameters);
    const auto end = monotonic_ns();
    ++status.frames_inferred;
    status.inference_ms = static_cast<float>(end - begin) / 1.0e6F;
    status.frame_age_ms = static_cast<float>(end - frame.timestamp_ns) / 1.0e6F;
    return true;
  }
}  // namespace sranipatch
