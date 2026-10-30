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

  std::vector<std::uint8_t> encode_osc(std::span<const float> values);

  class OscSender
  {
  public:
    OscSender(const std::string& host, std::uint16_t port);
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
