#pragma once

#include "sranipatch/protocol.hpp"

#include <filesystem>
#include <memory>
#include <span>

namespace sranipatch
{
  class HeadPreview
  {
  public:
    explicit HeadPreview(const std::filesystem::path& path);
    ~HeadPreview();
    HeadPreview(const HeadPreview&) = delete;
    HeadPreview& operator=(const HeadPreview&) = delete;
    unsigned int render(std::span<const Parameter> parameters, int width, int height);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
  };
}
