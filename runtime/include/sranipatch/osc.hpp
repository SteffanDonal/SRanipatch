#pragma once
#include "sranipatch/protocol.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sranipatch
{
  inline constexpr std::string_view face_tracking_address = "/tracking/face/v1";
  inline constexpr std::string_view legacy_osc_address = "/sranipatch/parameters";

  inline constexpr std::string_view osc_address(bool output_vrc_native)
  {
    return output_vrc_native ? face_tracking_address : legacy_osc_address;
  }

  std::vector<std::uint8_t> encode_osc(std::span<const float> values, bool output_vrc_native = false);

  class OscSender
  {
  public:
    OscSender(const std::string& host, std::uint16_t port, bool output_vrc_native = false);
    ~OscSender();
    OscSender(const OscSender&) = delete;
    OscSender& operator=(const OscSender&) = delete;
    bool send(std::span<const float> values);
    void refresh();
    bool waiting_for_peer() const;
    const OscDestination& destination() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
  };
}
