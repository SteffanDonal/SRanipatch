#pragma once

struct GLFWwindow;
struct ImFont;

namespace sranipatch
{
  void configure_windows_chrome(GLFWwindow* window);
  float windows_caption_height(GLFWwindow* window);
  ImFont* load_windows_caption_font();
  void draw_windows_caption(GLFWwindow* window, ImFont* font);
}
