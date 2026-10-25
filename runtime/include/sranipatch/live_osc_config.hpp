#pragma once
#include "sranipatch/osc.hpp"

#include <chrono>
#include <filesystem>
#include <optional>

namespace sranipatch
{
  struct OscOverrides
  {
    bool host {};
    bool port {};
    bool address {};
  };

  class LiveOscConfig
  {
  public:
    LiveOscConfig(std::filesystem::path path, Config defaults, OscOverrides overrides);
    bool poll(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());

    const Config& config() const
    {
      return config_;
    }

    OscSender& sender()
    {
      return *sender_;
    }

  private:
    struct Snapshot
    {
      std::optional<std::string> text;
      std::string error;
      bool operator==(const Snapshot&) const = default;
    };

    Snapshot read() const;
    void apply(const Snapshot& snapshot);
    std::filesystem::path path_;
    Config defaults_;
    OscOverrides overrides_;
    Config config_;
    std::unique_ptr<OscSender> sender_;
    Snapshot observed_;
    std::chrono::steady_clock::time_point next_check_ {};
    std::chrono::steady_clock::time_point changed_at_ {};
    bool pending_ {};
  };
}  // namespace sranipatch
