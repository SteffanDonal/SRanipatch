#pragma once
#include "sof/frame.hpp"
#include "sof/protocol.hpp"

#include <memory>
#include <stop_token>

namespace sof
{
  class Capture
  {
  public:
    virtual ~Capture() = default;
    // False means timeout; disconnection and configuration errors throw.
    virtual bool read(Frame& frame, std::stop_token stop) = 0;
  };

  // Construct and destroy on the capture thread (Media Foundation COM affinity).
  std::unique_ptr<Capture> open_capture();
}  // namespace sof
