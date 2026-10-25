#pragma once

#include <csignal>
#include <filesystem>
#include <functional>
#include <memory>

namespace sranipatch
{
  class Desktop
  {
  public:
    explicit Desktop(const std::filesystem::path& font);
    ~Desktop();
    Desktop(const Desktop&) = delete;
    Desktop& operator=(const Desktop&) = delete;
    void run(const std::function<void()>& draw, const volatile std::sig_atomic_t& interrupted);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
  };
}  // namespace sranipatch
