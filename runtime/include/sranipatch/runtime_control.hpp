#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>

namespace sranipatch
{
  // NOLINTNEXTLINE(performance-enum-size): Fixed-width field in the runtime control protocol.
  enum class OverlayState : std::uint32_t
  {
    stopped,
    starting,
    running,
    closed,
    failed
  };

  struct DriverControl
  {
    std::uint64_t session_id {}, heartbeat_ns {};
    bool show_overlay {};
    std::uint64_t overlay_revision {};
    std::uint64_t retry_client {};
    std::array<char, 256> message {};
  };

  struct ServerControl
  {
    std::uint64_t session_id {}, heartbeat_ns {};
    bool managed {};
    std::uint64_t overlay_revision {};
    OverlayState overlay { OverlayState::stopped };
    std::array<char, 256> message {};
  };

  struct RuntimeControl
  {
    DriverControl driver;
    ServerControl server;
  };

  // Independent of tracking snapshots: driver status survives a server restart.
  class ControlChannel
  {
  public:
    explicit ControlChannel(std::string name = "sranipatch");
    ~ControlChannel();
    ControlChannel(const ControlChannel&) = delete;
    ControlChannel& operator=(const ControlChannel&) = delete;
    RuntimeControl read();
    void publish_driver(const DriverControl& state);
    void publish_server(const ServerControl& state);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
  };
}
