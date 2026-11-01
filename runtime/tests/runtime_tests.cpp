#include "runtime.hpp"

#include "sranipatch/process.hpp"
#include "sranipatch/protocol.hpp"
#include "sranipatch/runtime_control.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace
{
  using namespace std::chrono_literals;

  void require(bool condition, const char* message)
  {
    if (!condition)
      throw std::runtime_error(message);
  }

  std::filesystem::path log_path(const std::string& name)
  {
    return std::filesystem::temp_directory_path() / (name + ".log");
  }

  int child(int argc, char** argv)
  {
    std::string name;
    sranipatch::ProcessLifetime lifetime;
    for (int i = 2; i + 1 < argc; ++i)
    {
      const std::string_view option(argv[i]);
      if (option == "--ipc")
        name = argv[++i];
      else if (option == "--lifetime")
        lifetime = sranipatch::ProcessLifetime(argv[++i]);
    }
    {
      std::ofstream log(log_path(name), std::ios::app);
      log << "start\n";
    }
    if (name.find("crash") != std::string::npos)
      return 1;
    if (name.find("close") != std::string::npos)
      return 2;
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (!lifetime.stop_requested() && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(10ms);
    std::ofstream log(log_path(name), std::ios::app);
    log << "stop\n";
    return lifetime.stop_requested() ? 0 : 1;
  }

  struct Session
  {
    std::string name;
    sranipatch::ControlChannel channel;
    sranipatch::DriverControl driver;
    bool previous_overlay {};

    explicit Session(std::string_view mode)
      : name("runtime-" + std::string(mode) + "-" + std::to_string(sranipatch::monotonic_ns())), channel(name)
    {
      driver.session_id = sranipatch::monotonic_ns();
    }

    ~Session()
    {
      std::error_code ignored;
      std::filesystem::remove(log_path(name), ignored);
    }

    void tick()
    {
      if (driver.show_overlay != previous_overlay)
      {
        ++driver.overlay_revision;
        previous_overlay = driver.show_overlay;
      }
      driver.heartbeat_ns = sranipatch::monotonic_ns();
      channel.publish_driver(driver);
    }

    template <typename Predicate>
    void wait(Predicate predicate)
    {
      const auto deadline = std::chrono::steady_clock::now() + 6s;
      do
      {
        tick();
        if (predicate(channel.read().server))
          return;
        std::this_thread::sleep_for(20ms);
      }
      while (std::chrono::steady_clock::now() < deadline);
      throw std::runtime_error("Timed out waiting for runtime state");
    }

    unsigned log_count(std::string_view event) const
    {
      std::ifstream input(log_path(name));
      std::string line;
      unsigned count {};
      while (std::getline(input, line))
        count += line == event ? 1U : 0U;
      return count;
    }
  };

  void client_lifetime_is_independent(const std::filesystem::path& executable)
  {
    Session session("wait");
    sranipatch::ServerRuntime runtime(session.name, false, executable);
    session.wait([](const auto& server) { return server.session_id != 0; });
    require(session.log_count("start") == 0, "Disabled overlay launched a client");
    session.driver.show_overlay = true;
    session.wait([&](const auto&) { return session.log_count("start") == 1; });
    session.driver.show_overlay = false;
    session.wait([&](const auto& server) {
      return server.overlay == sranipatch::OverlayState::stopped && session.log_count("stop") == 1;
    });
    const auto previous = session.channel.read().server.heartbeat_ns;
    session.driver.show_overlay = true;
    session.wait([&](const auto&) { return session.log_count("start") == 2; });
    session.driver.session_id = 0;
    session.wait([&](const auto& server) {
      return server.overlay == sranipatch::OverlayState::stopped && session.log_count("stop") == 2;
    });
    const auto server = session.channel.read().server;
    require(!server.managed && server.heartbeat_ns > previous, "Losing SteamVR stopped the manual server");
  }

  void crash_recovery_is_bounded(const std::filesystem::path& executable)
  {
    Session session("crash");
    sranipatch::ServerRuntime runtime(session.name, true, executable);
    session.driver.show_overlay = true;
    session.wait([](const auto& server) { return server.overlay == sranipatch::OverlayState::failed; });
    require(session.log_count("start") == 2, "Client recovery did not stop after one retry");
    require(session.channel.read().server.message[0] != '\0', "Client failure has no status message");
    ++session.driver.retry_client;
    session.wait([&](const auto& server) {
      return server.overlay == sranipatch::OverlayState::failed && session.log_count("start") == 4;
    });
  }

  void explicit_close_is_not_a_crash(const std::filesystem::path& executable)
  {
    Session session("close");
    {
      sranipatch::ServerRuntime runtime(session.name, true, executable);
      session.driver.show_overlay = true;
      session.wait([](const auto& server) { return server.overlay == sranipatch::OverlayState::closed; });
      require(session.log_count("start") == 1, "Explicit close restarted the client");
      ++session.driver.overlay_revision;
      session.wait([&](const auto& server) {
        return server.overlay == sranipatch::OverlayState::closed
          && server.overlay_revision == session.driver.overlay_revision
          && session.log_count("start") == 2;
      });
    }
    require(session.channel.read().server.heartbeat_ns == 0, "Stopped server still advertises availability");
  }
}

// NOLINTNEXTLINE(bugprone-exception-escape)
int main(int argc, char** argv)
{
  try
  {
    if (argc > 1 && std::string_view(argv[1]) == "--overlay")
      return child(argc, argv);
    const auto executable = std::filesystem::absolute(argv[0]);
    client_lifetime_is_independent(executable);
    crash_recovery_is_bounded(executable);
    explicit_close_is_not_a_crash(executable);
    std::cout << "Runtime lifecycle tests passed\n";
    return 0;
  }
  catch (const std::exception& error)
  {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
