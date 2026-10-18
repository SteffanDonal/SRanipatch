#pragma once

#include "sof/frame.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>

namespace sof
{
  enum class DatasetPixels : std::uint32_t
  {
    luma8 = 1,
    yuyv = 2
  };

  class DatasetWriter
  {
  public:
    DatasetWriter(const std::filesystem::path& path, std::uint32_t width, std::uint32_t height,
      DatasetPixels pixels = DatasetPixels::luma8);
    void write(const Frame& frame);
    std::uint64_t frame_count() const noexcept;

  private:
    std::ofstream file_;
    std::uint32_t width_ {};
    std::uint32_t height_ {};
    DatasetPixels pixels_ {};
    std::uint64_t frame_count_ {};
  };

  class DatasetReader
  {
  public:
    explicit DatasetReader(const std::filesystem::path& path);
    bool read(Frame& frame);
    std::uint32_t width() const noexcept;
    std::uint32_t height() const noexcept;

  private:
    std::ifstream file_;
    std::uint32_t width_ {};
    std::uint32_t height_ {};
    DatasetPixels pixels_ {};
  };
}  // namespace sof
