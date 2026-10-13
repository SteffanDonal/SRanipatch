#pragma once
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <stop_token>
#include <vector>

namespace sof
{
  struct Frame
  {
    std::uint32_t width {};
    std::uint32_t height {};
    std::uint64_t sequence {};
    std::uint64_t timestamp_ns {};
    std::vector<std::uint8_t> rgb;
  };

  // Producer and consumer recycle buffers by swapping; slow inference skips old frames.
  class LatestFrame
  {
  public:
    void push(Frame& frame);
    bool pop(Frame& frame, std::stop_token stop);

  private:
    std::mutex mutex_;
    std::condition_variable_any ready_;
    Frame frame_;
    bool pending_ {};
  };

  void yuyv_to_rgb(const std::uint8_t* source, std::uint32_t stride, Frame& frame);
}  // namespace sof
