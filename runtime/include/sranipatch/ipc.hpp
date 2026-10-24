#pragma once
#include "sranipatch/frame.hpp"
#include "sranipatch/protocol.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace sranipatch
{
  // One server per name. All access uses a crash-recoverable OS process lock;
  // snapshots contain no pointers, C++ atomics, or process-local synchronization objects.
  class SharedMemory
  {
  public:
    enum class Role : std::uint8_t
    {
      server,
      client
    };
    SharedMemory(std::string name, Role role);
    ~SharedMemory();
    SharedMemory(const SharedMemory&) = delete;
    SharedMemory& operator=(const SharedMemory&) = delete;
    bool read(Status& status);
    bool publish(const Status& status);
    // Preview copies are bounded and independent of status publication. False
    // means contention or no new frame; inference never waits for the GUI.
    bool publish_frame(const Frame& frame);
    bool read_frame(Frame& frame);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
  };
}  // namespace sranipatch
