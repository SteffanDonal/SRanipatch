#pragma once

#include "sof/lip.hpp"
#include "sof/protocol.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace sof
{
  struct HeadVertex
  {
    std::array<float, 3> position {};
    std::array<float, 3> normal {};
  };

  class HeadModel
  {
  public:
    explicit HeadModel(const std::filesystem::path& path);
    void deform(std::span<const Parameter> parameters);

    std::span<const HeadVertex> vertices() const
    {
      return deformed_;
    }

    std::span<const std::uint32_t> indices() const
    {
      return indices_;
    }

  private:
    std::vector<HeadVertex> base_, deformed_;
    std::vector<std::uint32_t> indices_;
    std::array<std::vector<HeadVertex>, lip_parameter_count> targets_;
  };
}  // namespace sof
