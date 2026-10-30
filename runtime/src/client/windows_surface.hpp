#pragma once
#include <memory>
#include <utility>
struct GLFWwindow;

namespace sranipatch
{
  class WindowsSurface
  {
  public:
    explicit WindowsSurface(GLFWwindow* window);
    ~WindowsSurface();
    WindowsSurface(const WindowsSurface&) = delete;
    WindowsSurface& operator=(const WindowsSurface&) = delete;
    void poll();
    void stop();
    std::pair<int, int> begin_frame();
    void bind_target(int width, int height);
    void present();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
  };
}
