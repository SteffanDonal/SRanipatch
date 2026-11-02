#include "tracking.hpp"

#include "sranipatch/capture.hpp"
#include "sranipatch/diagnostics.hpp"

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
            std::uint64_t attempt {};
            while (!stop.stop_requested())
            {
              try
              {
                log_event("camera.open.begin", "attempt=", ++attempt);
                auto camera = open_capture();
                log_event("camera.open.end", "attempt=", attempt);
                connected.store(true, std::memory_order_release);
                std::cout << "Camera connected\n";
                Frame frame;
                auto last_frame_time = monotonic_ns();
                auto summary_time = last_frame_time;
                std::uint64_t reads {}, empty_reads {};
                bool first_frame = true;
                while (!stop.stop_requested())
                {
                  ++reads;
                  const bool received = camera->read(frame, stop);
                  if (!received)
                    ++empty_reads;
                  if (received && !stop.stop_requested())
                  {
                    if (first_frame)
                    {
                      log_event("camera.first_frame", "attempt=", attempt, " width=", frame.width,
                        " height=", frame.height);
                      first_frame = false;
                    }
                    last_frame_time = monotonic_ns();
                    frame.sequence = captured.fetch_add(1, std::memory_order_relaxed) + 1;
                    latest.push(frame);
                  }
                  const auto now = monotonic_ns();
                  if (now - summary_time >= 5'000'000'000ULL)
                  {
                    log_summary("camera.read.summary", "attempt=", attempt, " reads=", reads,
                      " empty=", empty_reads, " captured=", captured.load(std::memory_order_relaxed),
                      " last_frame_age_ms=", (now - last_frame_time) / 1'000'000ULL);
                    summary_time = now;
                  }
                  if (monotonic_ns() - last_frame_time > 5'000'000'000ULL)
                  {
                    log_event("camera.frame_timeout", "attempt=", attempt, " reads=", reads, " empty=", empty_reads);
                    throw CaptureUnavailable(
                      "Camera has not delivered a valid frame for five seconds"
                    );
                  }
                }
                log_event("camera.close.begin", "reason=stop attempt=", attempt);
                camera.reset();
                log_event("camera.close.end", "attempt=", attempt);
              }
              catch (const CaptureUnavailable& failure)
              {
                connected.store(false, std::memory_order_release);
                log_event("camera.unavailable", "attempt=", attempt, " error=", failure.what(),
                  " new_arrival_required=", failure.new_arrival_required(), " stopping=", stop.stop_requested());
                if (!stop.stop_requested())
                {
                  std::cerr << "Camera unavailable: " << failure.what()
                            << "; waiting for USB device\n";
                  log_event("camera.device_wait.begin", "attempt=", attempt,
                    " new_arrival_required=", failure.new_arrival_required());
                  wait_for_capture_device(stop, failure.new_arrival_required());
                  log_event("camera.device_wait.end", "attempt=", attempt, " stopping=", stop.stop_requested());
                }
              }
            }
          }
          catch (const std::exception& failure)
          {
            log_event("camera.fault", "error=", failure.what());
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
      if (capture_)
        log_event("tracking.capture_stop", "reason=waiting_for_steam_link");
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
    {
      log_event("tracking.capture_start");
      capture_ = std::make_unique<CaptureRun>(captured_);
    }
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
}
