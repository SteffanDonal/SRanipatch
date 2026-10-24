#pragma once
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <stop_token>
#include <vector>

namespace sranipatch
{
  struct Frame
  {
    std::uint32_t width {};
    std::uint32_t height {};
    std::uint64_t sequence {};
    std::uint64_t timestamp_ns {};
    std::vector<std::uint8_t> luma;
    std::vector<std::uint8_t> yuyv;
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

  void copy_yuyv_luma(const std::uint8_t* source, std::ptrdiff_t stride, Frame& frame);
}  // namespace sranipatch
