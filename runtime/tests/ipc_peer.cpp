#include "sof/ipc.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

int main(int argc, char** argv)
{
  try
  {
    if (argc != 2)
      throw std::runtime_error("IPC peer needs a channel name");
    sof::SharedMemory client(argv[1], sof::SharedMemory::Role::client);
    sof::Status status;
    sof::Controls controls;
    if (!client.read(status, controls) || status.frames_inferred != 42 || status.parameters[0].value != 0.75F || status.parameter_count != 1)
      throw std::runtime_error("Child did not receive the server snapshot");
    std::uint64_t revision {};
    if (!client.control(true, false, revision) || revision != 1)
      throw std::runtime_error("Child could not submit controls");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline)
    {
      if (client.read(status, controls) && status.applied_revision == revision && status.state == sof::ServerState::paused && status.osc_enabled == 0)
        return 0;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    throw std::runtime_error("Parent did not acknowledge controls");
  }
  catch (const std::exception& error)
  {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
