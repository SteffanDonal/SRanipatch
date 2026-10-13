#pragma once
#include "sof/frame.hpp"
#include "sof/protocol.hpp"

#include <memory>
#include <string>

namespace sof
{
  // One server per name. All access uses a crash-recoverable OS process lock;
  // snapshots contain no pointers, C++ atomics, or process-local synchronization objects.
  class SharedMemory
  {
  public:
    enum class Role
    {
      server,
      client
    };
    SharedMemory(std::string name, Role role);
    ~SharedMemory();
    SharedMemory(const SharedMemory&) = delete;
    SharedMemory& operator=(const SharedMemory&) = delete;
    bool read(Status& status, Controls& controls);
    bool publish(const Status& status, Controls& controls);
    bool control(bool paused, bool osc_enabled, std::uint64_t& revision);
    // Preview copies are bounded and independent of status publication. False
    // means contention or no new frame; inference never waits for the GUI.
    bool publish_frame(const Frame& frame);
    bool read_frame(Frame& frame);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
  };
}  // namespace sof
