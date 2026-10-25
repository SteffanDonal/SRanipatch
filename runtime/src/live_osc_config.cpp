#include "sranipatch/live_osc_config.hpp"

#include "sranipatch/config.hpp"

#include <fstream>
#include <stdexcept>
#include <utility>

namespace sranipatch
{
  LiveOscConfig::LiveOscConfig(std::filesystem::path path, Config defaults, OscOverrides overrides)
    : path_(std::move(path)), defaults_(defaults), overrides_(overrides), config_(defaults),
      observed_(read())
  {
    apply(observed_);
  }

  LiveOscConfig::Snapshot LiveOscConfig::read() const
  {
    if (path_.empty())
      return {};
    try
    {
      if (!std::filesystem::exists(path_))
        return {};
      std::ifstream input(path_);
      if (!input)
        throw std::runtime_error("Cannot open configuration");
      std::string contents;
      std::array<char, 4096> buffer {};
      while (input.read(buffer.data(), static_cast<std::streamsize>(buffer.size())) || input.gcount() > 0)
        contents.append(buffer.data(), static_cast<std::size_t>(input.gcount()));
      if (!input.eof())
        throw std::runtime_error("Cannot read configuration");
      return { std::move(contents), {} };
    }
    catch (const std::exception& error)
    {
      return { {}, error.what() };
    }
  }

  void LiveOscConfig::apply(const Snapshot& snapshot)
  {
    if (!snapshot.error.empty())
      throw std::runtime_error(snapshot.error);
    auto candidate = snapshot.text ? parse_osc_config(*snapshot.text, defaults_) : defaults_;
    if (overrides_.host)
      candidate.osc_host = defaults_.osc_host;
    if (overrides_.port)
      candidate.osc_port = defaults_.osc_port;
    if (overrides_.address)
      candidate.osc_address = defaults_.osc_address;
    if (sender_ && candidate.osc_host == config_.osc_host && candidate.osc_port == config_.osc_port && candidate.osc_address == config_.osc_address)
      return;
    auto sender =
      std::make_unique<OscSender>(candidate.osc_host.data(), static_cast<std::uint16_t>(candidate.osc_port),
        candidate.osc_address.data());
    sender_ = std::move(sender);
    config_ = candidate;
  }

  bool LiveOscConfig::poll(std::chrono::steady_clock::time_point now)
  {
    using namespace std::chrono_literals;
    if (path_.empty() || now < next_check_)
      return false;
    next_check_ = now + 100ms;
    auto snapshot = read();
    if (snapshot != observed_)
    {
      observed_ = std::move(snapshot);
      changed_at_ = now;
      pending_ = true;
    }
    if (!pending_ || now - changed_at_ < 200ms)
      return false;
    pending_ = false;
    if (!observed_.text && observed_.error.empty())
      throw std::runtime_error("Configuration file is missing");
    apply(observed_);
    return true;
  }
}  // namespace sranipatch
