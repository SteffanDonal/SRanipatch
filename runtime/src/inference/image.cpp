#include "sranipatch/lip.hpp"

#include <opencv2/core/hal/intrin.hpp>

#include <algorithm>
#include <array>
#include <stdexcept>
#include <type_traits>

namespace sranipatch
{
  namespace
  {
    template <class T>
    T median5(T a, T b, T c, T d, T e)
    {
      const auto order = [](T& left, T& right) {
        const auto previous = left;
        if constexpr (std::is_integral_v<T>)
        {
          left = std::min(left, right);
          right = std::max(previous, right);
        }
        else
        {
          left = cv::v_min(left, right);
          right = cv::v_max(previous, right);
        }
      };
      order(a, b);
      order(d, e);
      order(c, e);
      order(c, d);
      order(b, e);
      order(a, d);
      order(a, c);
      order(b, d);
      order(b, c);
      return c;
    }
  }  // namespace

  void pseudo_separable_median_5x5(std::span<const std::uint8_t> input, std::span<std::uint8_t> output,
    std::span<std::uint8_t> scratch, std::size_t width, std::size_t height)
  {
    if (!width || !height || input.size() / width != height || input.size() % width || output.size() != input.size() || scratch.size() != input.size() || input.data() == output.data() || input.data() == scratch.data() || output.data() == scratch.data())
      throw std::runtime_error("Separable median requires three separate, matching image buffers");

    for (std::size_t y = 0; y < height; ++y)
    {
      const auto* row = input.data() + y * width;
      auto* dest = scratch.data() + y * width;
      std::size_t x {};
      while (x < width)
      {
#if CV_SIMD128
        if (x >= 2 && width - x >= 18)
        {
          cv::v_store(dest + x,
            median5(cv::v_load(row + x - 2), cv::v_load(row + x - 1), cv::v_load(row + x),
              cv::v_load(row + x + 1), cv::v_load(row + x + 2)));
          x += 16;
          continue;
        }
#endif
        dest[x] = median5(row[x < 2 ? 0 : x - 2], row[x ? x - 1 : 0], row[x],
          row[std::min(x + 1, width - 1)], row[std::min(x + 2, width - 1)]);
        ++x;
      }
    }
    for (std::size_t y = 0; y < height; ++y)
    {
      const std::array<const std::uint8_t*, 5> rows {
        scratch.data() + (y < 2 ? 0 : y - 2) * width, scratch.data() + (y ? y - 1 : 0) * width,
        scratch.data() + y * width, scratch.data() + std::min(y + 1, height - 1) * width,
        scratch.data() + std::min(y + 2, height - 1) * width
      };
      auto* dest = output.data() + y * width;
      std::size_t x {};
#if CV_SIMD128
      for (; width - x >= 16; x += 16)
        cv::v_store(dest + x,
          median5(cv::v_load(rows[0] + x), cv::v_load(rows[1] + x), cv::v_load(rows[2] + x),
            cv::v_load(rows[3] + x), cv::v_load(rows[4] + x)));
#endif
      for (; x < width; ++x)
        dest[x] = median5(rows[0][x], rows[1][x], rows[2][x], rows[3][x], rows[4][x]);
    }
  }
}
