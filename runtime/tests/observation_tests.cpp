#include "observation.hpp"

#include <iostream>
#include <stdexcept>
#ifndef _WIN32
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace
{
  void require(bool condition, const char* message)
  {
    if (!condition)
      throw std::runtime_error(message);
  }

  struct Channel
  {
    std::string name { "observation-" + std::to_string(sranipatch::monotonic_ns()) };

    ~Channel()
    {
#ifndef _WIN32
      const auto base = "/sranipatch-" + std::to_string(getuid()) + "-" + name;
      shm_unlink(base.c_str());
      shm_unlink((base + "-owner").c_str());
#endif
    }
  };

  void independent_freshness()
  {
    const Channel channel;
    sranipatch::SharedMemory server(channel.name, sranipatch::SharedMemory::Role::server);
    std::uint64_t now {};
    sranipatch::Observation observation(channel.name, [&] { return now; });
    sranipatch::Status status;
    status.session_id = 1;
    status.state = sranipatch::ServerState::running;
    status.heartbeat_ns = 1'000'000'000;
    status.frames_inferred = 1;
    status.parameter_count = 1;
    status.parameters[0].value = 0.75F;
    sranipatch::Frame frame;
    frame.width = sranipatch::capture_width;
    frame.height = sranipatch::capture_height;
    frame.sequence = 1;
    frame.luma.assign(static_cast<std::size_t>(frame.width) * frame.height, 123);
    require(server.publish(status) && server.publish_frame(frame), "Cannot publish observation");
    now = 1'000'000'000;
    const auto& first = observation.update();
    require(first.available && first.camera_fresh && first.tracking_fresh, "Initial data is not live");

    status.heartbeat_ns = 1'250'000'000;
    frame.sequence = 2;
    require(server.publish(status) && server.publish_frame(frame), "Cannot publish camera progress");
    now = 1'250'000'000;
    const auto& stalled = observation.update();
    require(stalled.available && stalled.camera_fresh && !stalled.tracking_fresh,
      "Camera progress must not hide an inference stall");
    require(stalled.status.parameters[0].value == 0.75F, "Stale parameters must remain visible");

    status.heartbeat_ns = 1'500'000'000;
    status.frames_inferred = 2;
    require(server.publish(status), "Cannot publish inference progress");
    now = 1'500'000'000;
    const auto& resumed = observation.update();
    require(resumed.available && !resumed.camera_fresh && resumed.tracking_fresh,
      "Inference progress must not hide a camera stall");
    require(!resumed.frame.luma.empty(), "Stale camera image must remain visible");
  }

  void session_replacement()
  {
    const Channel channel;
    auto server =
      std::make_unique<sranipatch::SharedMemory>(channel.name, sranipatch::SharedMemory::Role::server);
    std::uint64_t now {};
    sranipatch::Observation observation(channel.name, [&] { return now; });
    sranipatch::Status status;
    status.session_id = 1;
    status.heartbeat_ns = 1'000'000'000;
    status.frames_inferred = 1;
    status.parameter_count = 1;
    sranipatch::Frame frame;
    frame.width = sranipatch::capture_width;
    frame.height = sranipatch::capture_height;
    frame.sequence = 1;
    frame.luma.assign(static_cast<std::size_t>(frame.width) * frame.height, 123);
    require(server->publish(status) && server->publish_frame(frame), "Cannot publish first session");
    now = 1'000'000'000;
    observation.update();

    server.reset();
    server = std::make_unique<sranipatch::SharedMemory>(channel.name, sranipatch::SharedMemory::Role::server);
    status = {};
    status.session_id = 2;
    status.heartbeat_ns = 1'100'000'000;
    require(server->publish(status), "Cannot publish replacement session");
    now = 1'100'000'000;
    const auto& waiting = observation.update();
    require(waiting.session_changed && waiting.status.parameter_count == 0 && waiting.frame.luma.empty(),
      "New session must clear previous observations");
    require(!waiting.camera_fresh && !waiting.tracking_fresh, "New session must wait for its own data");
  }

  void reconnect_and_fault()
  {
    const Channel channel;
    std::uint64_t now {};
    sranipatch::Observation observation(channel.name, [&] { return now; });
    now = 1'000'000'000;
    require(!observation.update().available, "Missing server must be offline");
    sranipatch::SharedMemory server(channel.name, sranipatch::SharedMemory::Role::server);
    sranipatch::Status status;
    status.session_id = 1;
    status.heartbeat_ns = 1'500'000'000;
    require(server.publish(status), "Cannot publish server startup");
    now = 1'500'000'000;
    require(!observation.update().available, "Reconnect must respect its retry interval");
    now = 2'000'000'000;
    require(observation.update().available, "Client must reconnect automatically");
    now = 3'500'000'000;
    require(!observation.update().available, "Expired heartbeat must mark server offline");
    status.heartbeat_ns = 3'600'000'000;
    require(server.publish(status), "Cannot publish renewed heartbeat");
    now = 3'600'000'000;
    require(!observation.update().available, "Expired mapping must wait before reopening");
    now = 4'500'000'000;
    require(observation.update().available, "Renewed heartbeat must permit reconnection");
    status.state = sranipatch::ServerState::fault;
    sranipatch::set_text(status.error, "Inference failed");
    require(server.publish(status), "Cannot publish fault");
    now = 4'600'000'000;
    const auto& fault = observation.update();
    require(!fault.available && fault.message == "Inference failed", "Fault must retain its useful error");
    status = {};
    status.session_id = 2;
    status.heartbeat_ns = 5'600'000'000;
    require(server.publish(status), "Cannot publish recovered server");
    now = 5'600'000'000;
    const auto& recovered = observation.update();
    require(recovered.available && recovered.session_changed && recovered.message.empty(),
      "Client must recover after a server fault");
    status.state = sranipatch::ServerState::stopped;
    require(server.publish(status), "Cannot publish stopped server");
    now = 5'700'000'000;
    require(!observation.update().available, "Stopped server must be offline immediately");
  }

  void heartbeat_during_observation()
  {
    const Channel channel;
    sranipatch::SharedMemory server(channel.name, sranipatch::SharedMemory::Role::server);
    sranipatch::Status status;
    status.session_id = 1;
    status.heartbeat_ns = 1'000'000'001;
    require(server.publish(status), "Cannot publish heartbeat");
    std::uint64_t now = 1'000'000'000;
    sranipatch::Observation observation(channel.name, [&] { return now++; });
    require(observation.update().available, "Heartbeat newer than poll start must not disconnect server");
  }

  void osc_destination_updates_without_frames()
  {
    const Channel channel;
    sranipatch::SharedMemory server(channel.name, sranipatch::SharedMemory::Role::server);
    std::uint64_t now = 1'000'000'000;
    sranipatch::Observation observation(channel.name, [&] { return now; });
    sranipatch::Status status;
    status.session_id = 1;
    status.heartbeat_ns = now;
    sranipatch::set_text(status.config.osc_host, "192.0.2.1");
    sranipatch::set_text(status.osc_destination.host, "10.35.78.26");
    status.osc_destination.port = 9337;
    status.osc_destination.ready = 1;
    require(server.publish(status), "Cannot publish resolved destination");
    const auto& connected = observation.update();
    require(connected.available && connected.status.osc_destination.ready && std::string_view(connected.status.osc_destination.host.data()) == "10.35.78.26" && connected.status.osc_destination.port == 9337,
      "Client must read runtime destination from server status independently of configured host");
    now += 100'000'000;
    status.heartbeat_ns = now;
    status.osc_destination.ready = 0;
    require(server.publish(status), "Cannot publish disconnected destination");
    const auto& waiting = observation.update();
    require(waiting.available && !waiting.status.osc_destination.ready && std::string_view(waiting.status.osc_destination.host.data()) == "10.35.78.26",
      "Client must retain the server's last destination while waiting without camera frames");
    now += 100'000'000;
    status.session_id = 2;
    status.heartbeat_ns = now;
    status.osc_destination = {};
    require(server.publish(status), "Cannot publish restarted server");
    const auto& restarted = observation.update();
    require(restarted.session_changed && !restarted.status.osc_destination.host[0],
      "Last destination must not leak between server sessions");
  }
}  // namespace

// Exceptions from the body are caught below; clang-tidy misidentifies MSVC exception inheritance.
// NOLINTNEXTLINE(bugprone-exception-escape)
int main()
{
  try
  {
    independent_freshness();
    session_replacement();
    reconnect_and_fault();
    heartbeat_during_observation();
    osc_destination_updates_without_frames();
    std::cout << "Client observation passed\n";
    return 0;
  }
  catch (const std::exception& error)
  {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
