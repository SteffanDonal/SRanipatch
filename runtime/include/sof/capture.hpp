#pragma once
#include "sof/frame.hpp"
#include "sof/protocol.hpp"

#include <memory>
#include <stdexcept>
#include <stop_token>
#include <string>

namespace sof
{
  class CaptureUnavailable : public std::runtime_error
  {
    bool new_arrival_required_;

  public:
    explicit CaptureUnavailable(const std::string& message, bool new_arrival_required = true)
      : std::runtime_error(message), new_arrival_required_(new_arrival_required) {}

    bool new_arrival_required() const noexcept
    {
      return new_arrival_required_;
    }
  };

  class Capture
  {
  public:
    virtual ~Capture() = default;
    // False means timeout or a discarded malformed sample. Disconnection,
    // configuration and transport errors throw.
    virtual bool read(Frame& frame, std::stop_token stop) = 0;
  };

  // Construct and destroy on the capture thread (Media Foundation COM affinity).
  std::unique_ptr<Capture> open_capture();
  // Sleeps until the fixed VIVE capture device is present (or newly arrives), or stop is requested.
  // Implementations use native device notifications rather than periodic scans.
  void wait_for_capture_device(std::stop_token stop, bool new_arrival_required);
}  // namespace sof
