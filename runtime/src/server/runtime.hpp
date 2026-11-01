#pragma once

#include <filesystem>
#include <string>
#include <thread>

namespace sranipatch
{
  class ServerRuntime
  {
  public:
    ServerRuntime(std::string ipc, bool managed, std::filesystem::path client = {});

  private:
    std::jthread worker_;
  };
}
