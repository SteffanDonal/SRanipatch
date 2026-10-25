#pragma once

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
    explicit Overlay(SurfaceSize size);
    ~Overlay();
    Overlay(const Overlay&) = delete;
    Overlay& operator=(const Overlay&) = delete;
    bool poll();
    bool visible() const;
    void bind_target() const;
    void submit();
    void wait_frame() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
  };
}  // namespace sranipatch
