#pragma once

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sranipatch
{
  class ChildProcess
  {
  public:
    ChildProcess();
    ~ChildProcess();
    ChildProcess(ChildProcess&&) noexcept;
    ChildProcess& operator=(ChildProcess&&) noexcept;
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    void start(const std::filesystem::path& executable, const std::vector<std::string>& arguments = {},
      const std::filesystem::path& working_directory = {});
    std::optional<int> poll() noexcept;
    bool active() const noexcept;
    void request_stop() noexcept;
    void stop(std::chrono::milliseconds timeout = std::chrono::seconds(3)) noexcept;

  private:
    struct State;
    std::unique_ptr<State> state_;
  };

  class ProcessLifetime
  {
  public:
    ProcessLifetime();
    explicit ProcessLifetime(std::string_view token);
    ~ProcessLifetime();
    ProcessLifetime(ProcessLifetime&&) noexcept;
    ProcessLifetime& operator=(ProcessLifetime&&) noexcept;
    ProcessLifetime(const ProcessLifetime&) = delete;
    ProcessLifetime& operator=(const ProcessLifetime&) = delete;

    bool stop_requested() const noexcept;

  private:
    struct State;
    std::unique_ptr<State> state_;
  };

  std::filesystem::path executable_directory();
  void cleanup_old_packages() noexcept;
}
