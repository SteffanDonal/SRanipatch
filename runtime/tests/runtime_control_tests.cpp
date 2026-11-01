#include "sranipatch/runtime_control.hpp"

#include "sranipatch/protocol.hpp"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <thread>
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

  void independent_writers()
  {
    const auto name = "test-control-" + std::to_string(sranipatch::monotonic_ns());
#ifndef _WIN32
    struct Cleanup
    {
      std::string name;

      ~Cleanup() { shm_unlink(name.c_str()); }
    } cleanup { "/sranipatch-control-" + std::to_string(getuid()) + "-" + name };
#endif
    sranipatch::ControlChannel driver(name);
    sranipatch::ControlChannel server(name);
    require(driver.read().driver.heartbeat_ns == 0, "New channel advertised a live driver");
    require(server.read().server.heartbeat_ns == 0, "New channel advertised a live server");
    std::exception_ptr failure;
    std::jthread writer([&] {
      try
      {
        sranipatch::ServerControl state;
        state.session_id = 42;
        state.managed = true;
        state.overlay = sranipatch::OverlayState::running;
        for (std::uint64_t i = 1; i <= 1000; ++i)
        {
          state.heartbeat_ns = i;
          state.overlay_revision = i;
          server.publish_server(state);
        }
      }
      catch (...)
      {
        failure = std::current_exception();
      }
    });
    sranipatch::DriverControl state;
    state.session_id = 7;
    state.show_overlay = true;
    sranipatch::set_text(state.message, "A retained failure message");
    for (std::uint64_t i = 1; i <= 1000; ++i)
    {
      state.heartbeat_ns = i;
      state.overlay_revision = i;
      driver.publish_driver(state);
      const auto snapshot = driver.read();
      require(snapshot.driver.heartbeat_ns == i, "Server overwrote driver state");
      require(snapshot.server.heartbeat_ns == snapshot.server.overlay_revision, "Torn server snapshot");
    }
    writer.join();
    if (failure)
      std::rethrow_exception(failure);
    const auto snapshot = server.read();
    require(snapshot.server.heartbeat_ns == 1000, "Driver overwrote server state");
    require(snapshot.driver.heartbeat_ns == 1000, "Driver update lost");
    require(snapshot.server.managed && snapshot.driver.show_overlay, "Flags lost");
    sranipatch::ControlChannel reconnect(name);
    require(reconnect.read().driver.message == state.message, "Reconnect reset driver status");
    server.publish_server({});
    require(driver.read().driver.message == state.message, "Server shutdown reset failure status");
  }
}

// NOLINTNEXTLINE(bugprone-exception-escape): Exceptions are caught below.
int main()
{
  try
  {
    independent_writers();
    return 0;
  }
  catch (const std::exception& error)
  {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
