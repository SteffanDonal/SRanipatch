#pragma once

#include "sranipatch/protocol.hpp"

#include <filesystem>

namespace sranipatch
{
  std::filesystem::path default_config_path();
  bool load_osc_config(const std::filesystem::path& path, Config& config);
}  // namespace sranipatch
