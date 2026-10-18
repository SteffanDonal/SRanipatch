#include "sof/lip.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>

namespace sof
{
  void median_filter_5x5(std::span<const std::uint8_t> input, std::span<std::uint8_t> output, std::size_t width,
    std::size_t height)
  {
    if (!width || !height || input.size() / width != height || input.size() % width || output.size() != input.size() || input.data() == output.data())
      throw std::runtime_error("Median filter requires separate, matching image buffers");

    for (std::size_t y = 0; y < height; ++y)
    {
      std::array<const std::uint8_t*, 5> rows {};
      for (int dy = -2; dy <= 2; ++dy)
      {
        const auto row = std::clamp(static_cast<std::ptrdiff_t>(y) + dy, std::ptrdiff_t { 0 },
          static_cast<std::ptrdiff_t>(height - 1));
        rows[static_cast<std::size_t>(dy + 2)] = input.data() + static_cast<std::size_t>(row) * width;
      }
      std::array<int, 256> fine {};
      std::array<int, 16> coarse {};
      for (const auto* row : rows)
      {
        for (int dx = -2; dx <= 2; ++dx)
        {
          const auto x = static_cast<std::size_t>(std::clamp(dx, 0, static_cast<int>(width - 1)));
          const auto value = row[x];
          ++fine[value];
          ++coarse[value >> 4];
        }
      }
      for (std::size_t x = 0; x < width; ++x)
      {
        int rank = 12;
        unsigned bucket {};
        while (rank >= coarse[bucket])
          rank -= coarse[bucket++];
        unsigned value = bucket << 4;
        while (rank >= fine[value])
          rank -= fine[value++];
        output[y * width + x] = static_cast<std::uint8_t>(value);
        if (x + 1 == width)
          break;
        const auto leaving = x < 2 ? 0 : x - 2;
        const auto entering = std::min(x + 3, width - 1);
        for (const auto* row : rows)
        {
          const auto old_value = row[leaving];
          const auto new_value = row[entering];
          --fine[old_value];
          --coarse[old_value >> 4];
          ++fine[new_value];
          ++coarse[new_value >> 4];
        }
      }
    }
  }
}  // namespace sof
