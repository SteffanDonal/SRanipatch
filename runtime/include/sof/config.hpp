#pragma once

#include "sof/protocol.hpp"

#include <filesystem>

namespace sof
{
  std::filesystem::path default_config_path();
  bool load_osc_config(const std::filesystem::path& path, Config& config);
}  // namespace sof
