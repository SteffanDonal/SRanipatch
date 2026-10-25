#pragma once
#include <filesystem>
#include <string>

namespace sranipatch
{
  std::string steam_link_peer(const std::filesystem::path& proc = "/proc");
}
