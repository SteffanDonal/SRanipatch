#pragma once

#include "sranipatch/protocol.hpp"

#include <span>

namespace sranipatch
{
  void draw_parameter_grid(std::span<const Parameter> parameters);
}
