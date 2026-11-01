#pragma once

#include <filesystem>
#include <memory>

namespace sranipatch
{
  struct SurfaceSize
  {
    int width;
    int height;
  };

  class Overlay
  {
  public:
    Overlay(SurfaceSize size, const std::filesystem::path& thumbnail);
    ~Overlay();
    Overlay(const Overlay&) = delete;
    Overlay& operator=(const Overlay&) = delete;
    bool poll();
    bool visible() const;
    bool closed_by_user() const;
    void bind_target() const;
    void submit();
    void wait_frame() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
  };
}
