#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace sranipatch
{
  // Transport for UVC extension unit 4, selector 2. Windows uses IKsControl;
  // Linux can supply UVCIOC_CTRL_QUERY without changing the device protocol.
  class TrackerExtension
  {
  public:
    virtual ~TrackerExtension() = default;
    virtual std::size_t buffer_size() const = 0;
    virtual void get(std::span<std::uint8_t> data) = 0;
    virtual void set(std::span<const std::uint8_t> data) = 0;
  };

  void enable_vive_tracker(TrackerExtension& extension);
  void disable_vive_tracker(TrackerExtension& extension);
}  // namespace sranipatch
