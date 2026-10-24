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
    if (!ready_.wait_for(lock, std::move(stop), std::chrono::milliseconds(50), [this] { return pending_; }))
      return false;
    std::swap(frame_, frame);
    pending_ = false;
    return true;
  }

  void copy_yuyv_luma(const std::uint8_t* source, std::ptrdiff_t stride, Frame& frame)
  {
    if (!frame.width || !frame.height || (stride >= 0 ? stride : -stride) < std::ptrdiff_t { frame.width } * 2)
      throw std::runtime_error("Invalid YUYV luminance row stride");
    frame.luma.resize(static_cast<std::size_t>(frame.width) * frame.height);
    frame.yuyv.resize(static_cast<std::size_t>(frame.width) * frame.height * 2);
    for (std::uint32_t y = 0; y < frame.height; ++y)
    {
      const auto* row = source + static_cast<std::ptrdiff_t>(y) * stride;
      auto* dest = frame.luma.data() + static_cast<std::size_t>(y) * frame.width;
      std::copy_n(row, static_cast<std::size_t>(frame.width) * 2,
        frame.yuyv.data() + static_cast<std::size_t>(y) * frame.width * 2);
      for (std::uint32_t x = 0; x < frame.width; ++x)
        dest[x] = row[static_cast<std::size_t>(x) * 2];
    }
  }
}  // namespace sof
