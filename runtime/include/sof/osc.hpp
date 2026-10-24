#pragma once
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sof
{
  std::vector<std::uint8_t> encode_osc(std::string_view address, std::span<const float> values);

  class OscSender
  {
  public:
    OscSender(const std::string& host, std::uint16_t port, std::string address);
    ~OscSender();
    OscSender(const OscSender&) = delete;
    OscSender& operator=(const OscSender&) = delete;
    bool send(std::span<const float> values);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
  };
}  // namespace sof
