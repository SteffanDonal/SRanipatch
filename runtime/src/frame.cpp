#include "sof/frame.hpp"

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
}  // namespace sof
