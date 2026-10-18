#include "sof/lip.hpp"

#include <opencv2/imgproc.hpp>

#include <limits>
#include <stdexcept>

namespace sof
{
  void median_filter_5x5(std::span<const std::uint8_t> input, std::span<std::uint8_t> output, std::size_t width,
    std::size_t height)
  {
    if (!width || !height || input.size() / width != height || input.size() % width || output.size() != input.size() || input.data() == output.data() || width > static_cast<std::size_t>(std::numeric_limits<int>::max()) || height > static_cast<std::size_t>(std::numeric_limits<int>::max()))
      throw std::runtime_error("Median filter requires separate, matching image buffers");

    const cv::Mat source(static_cast<int>(height), static_cast<int>(width), CV_8UC1,
      const_cast<std::uint8_t*>(input.data()));
    cv::Mat destination(static_cast<int>(height), static_cast<int>(width), CV_8UC1, output.data());
    cv::medianBlur(source, destination, 5);
  }
}  // namespace sof
