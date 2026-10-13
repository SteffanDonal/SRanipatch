#include "sof/frame.hpp"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <utility>

namespace sof
{
  void LatestFrame::push(Frame& frame)
  {
    {
      std::lock_guard lock(mutex_);
      std::swap(frame_, frame);
      pending_ = true;
    }
    ready_.notify_one();
  }

  bool LatestFrame::pop(Frame& frame, std::stop_token stop)
  {
    std::unique_lock lock(mutex_);
    if (!ready_.wait_for(lock, stop, std::chrono::milliseconds(50), [this] { return pending_; }))
      return false;
    std::swap(frame_, frame);
    pending_ = false;
    return true;
  }

  void yuyv_to_rgb(const std::uint8_t* source, std::uint32_t stride, Frame& frame)
  {
    if (frame.width % 2 != 0 || stride < frame.width * 2)
      throw std::runtime_error("YUYV requires even width and a valid row stride");
    frame.rgb.resize(static_cast<std::size_t>(frame.width) * frame.height * 3);
    auto clamp = [](int value) { return static_cast<std::uint8_t>(std::clamp(value, 0, 255)); };
    for (std::uint32_t y = 0; y < frame.height; ++y)
    {
      const auto* row = source + static_cast<std::size_t>(y) * stride;
      auto* dest = frame.rgb.data() + static_cast<std::size_t>(y) * frame.width * 3;
      for (std::uint32_t x = 0; x < frame.width; x += 2)
      {
        const int u = row[1] - 128, v = row[3] - 128;
        for (int pixel = 0; pixel < 2; ++pixel)
        {
          const int c = std::max(0, static_cast<int>(row[pixel * 2]) - 16);
          *dest++ = clamp((298 * c + 409 * v + 128) >> 8);
          *dest++ = clamp((298 * c - 100 * u - 208 * v + 128) >> 8);
          *dest++ = clamp((298 * c + 516 * u + 128) >> 8);
        }
        row += 4;
      }
    }
  }

  void yuyv_luma_to_rgb(const std::uint8_t* source, std::ptrdiff_t stride, Frame& frame)
  {
    if (!frame.width || !frame.height || (stride >= 0 ? stride : -stride) < frame.width * 2)
      throw std::runtime_error("Invalid YUYV luminance row stride");
    frame.rgb.resize(static_cast<std::size_t>(frame.width) * frame.height * 3);
    for (std::uint32_t y = 0; y < frame.height; ++y)
    {
      const auto* row = source + static_cast<std::ptrdiff_t>(y) * stride;
      auto* dest = frame.rgb.data() + static_cast<std::size_t>(y) * frame.width * 3;
      for (std::uint32_t x = 0; x < frame.width; ++x)
      {
        const auto luminance = row[x * 2];
        dest[x * 3] = dest[x * 3 + 1] = dest[x * 3 + 2] = luminance;
      }
    }
  }

  void resize_preview(const Frame& source, Frame& dest, std::uint32_t max_width, std::uint32_t max_height)
  {
    if (!source.width || !source.height || !max_width || !max_height || source.rgb.size() != static_cast<std::size_t>(source.width) * source.height * 3)
      throw std::runtime_error("Invalid RGB preview source");
    const auto scale = std::min({ 1.0, static_cast<double>(max_width) / source.width,
      static_cast<double>(max_height) / source.height });
    dest.width = std::max(1U, static_cast<std::uint32_t>(source.width * scale));
    dest.height = std::max(1U, static_cast<std::uint32_t>(source.height * scale));
    dest.sequence = source.sequence;
    dest.timestamp_ns = source.timestamp_ns;
    dest.rgb.resize(static_cast<std::size_t>(dest.width) * dest.height * 3);
    for (std::uint32_t y = 0; y < dest.height; ++y)
    {
      const auto sy = static_cast<std::uint64_t>(y) * source.height / dest.height;
      for (std::uint32_t x = 0; x < dest.width; ++x)
      {
        const auto sx = static_cast<std::uint64_t>(x) * source.width / dest.width;
        const auto from = static_cast<std::size_t>(sy * source.width + sx) * 3;
        const auto to = (static_cast<std::size_t>(y) * dest.width + x) * 3;
        std::copy_n(source.rgb.data() + from, 3, dest.rgb.data() + to);
      }
    }
  }
}  // namespace sof
