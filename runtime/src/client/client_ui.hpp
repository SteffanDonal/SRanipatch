#pragma once

#include <csignal>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>

namespace sranipatch
{
  enum class Presentation : std::uint8_t
  {
    desktop,
    overlay
  };

  class ClientUi
  {
  public:
    explicit ClientUi(const std::filesystem::path& font, Presentation presentation);
    ~ClientUi();
    ClientUi(const ClientUi&) = delete;
    ClientUi& operator=(const ClientUi&) = delete;
    void run(const std::function<void()>& draw, const volatile std::sig_atomic_t& interrupted,
      const std::function<bool()>& stop_requested = {});
    bool closed_by_user() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
  };
}
