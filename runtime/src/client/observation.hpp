#pragma once

#include "sranipatch/ipc.hpp"

#include <functional>

namespace sranipatch
{
  struct ObservationSnapshot
  {
    Status status;
    Frame frame;
    std::string message;
    bool available {};
    bool camera_fresh {};
    bool tracking_fresh {};
    bool session_changed {};
    bool frame_changed {};
  };

  class Observation
  {
  public:
    explicit Observation(std::string name, std::function<std::uint64_t()> clock = monotonic_ns);
    const ObservationSnapshot& update();

  private:
    std::string name_;
    std::function<std::uint64_t()> clock_;
    std::unique_ptr<SharedMemory> ipc_;
    ObservationSnapshot snapshot_;
    std::uint64_t retry_at_ {}, camera_at_ {}, tracking_at_ {};
  };
}
