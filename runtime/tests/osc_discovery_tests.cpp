#include "../src/steam_link.hpp"
#include "sranipatch/osc.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>

namespace
{
  std::string discovered_peer;
  unsigned lookups {};

  void require(bool condition, const char* message)
  {
    if (!condition)
      throw std::runtime_error(message);
  }
}  // namespace

namespace sranipatch
{
  // Replace only peer discovery; exercise the real sender and UDP sockets.
  std::string steam_link_peer(const std::filesystem::path&)
  {
    ++lookups;
    return discovered_peer;
  }
}

// NOLINTNEXTLINE(bugprone-exception-escape): Exceptions from the body are caught below.
int main()
{
  try
  {
    sranipatch::OscSender sender("", 9337);
    auto now = std::chrono::steady_clock::now();
    require(sender.waiting_for_peer() && !sender.destination().ready && !sender.destination().host[0],
      "Initial state must be waiting");
    sender.refresh(now);
    require(lookups == 1, "Discovery must respect the two-second interval");
    discovered_peer = "127.0.0.1";
    now += std::chrono::seconds(2);
    sender.refresh(now);
    require(!sender.waiting_for_peer() && sender.destination().ready && sender.destination().port == 9337 && std::string_view(sender.destination().host.data()) == discovered_peer,
      "Refresh without sending must resolve and report the destination");
    discovered_peer.clear();
    now += std::chrono::seconds(2);
    sender.refresh(now);
    require(sender.waiting_for_peer() && !sender.destination().ready && std::string_view(sender.destination().host.data()) == "127.0.0.1" && sender.send({}),
      "Disconnection must retain the last address and pause output without a send failure");
    discovered_peer = "127.0.0.2";
    now += std::chrono::seconds(2);
    sender.refresh(now);
    require(sender.destination().ready && std::string_view(sender.destination().host.data()) == discovered_peer,
      "Reconnection must replace the last address");
    std::cout << "OSC discovery lifecycle passed\n";
    return 0;
  }
  catch (const std::exception& error)
  {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
