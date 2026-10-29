#include "windows_chrome.hpp"
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <imgui.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace sranipatch
{
  namespace
  {
    constexpr auto maximize_pressed = L"SRanipatch.MaximizePressed";

    namespace caption
    {
      constexpr int reference_dpi = 96;
      constexpr int maximized_height = 28;
      constexpr int button_stride = 46;
      constexpr int button_count = 3;
      constexpr int maximize_index = 1;
      constexpr int close_index = 2;
      constexpr float button_gap = 1;
      constexpr float icon_size = 10;
      constexpr float hover_seconds = 0.1F;
      constexpr float press_seconds = 0.05F;
      constexpr float leave_seconds = 0.3F;
      constexpr float hover_exponent = 2.5F;
      constexpr float leave_exponent = 1.75F;
      constexpr ImU32 idle_background = IM_COL32(255, 255, 255, 0);
      constexpr ImU32 hover_background = IM_COL32(255, 255, 255, 15);
      constexpr ImU32 pressed_background = IM_COL32(255, 255, 255, 11);
      constexpr ImU32 close_idle_background = IM_COL32(196, 43, 28, 0);
      constexpr ImU32 close_hover_background = IM_COL32(196, 43, 28, 255);
      constexpr ImU32 close_pressed_background = IM_COL32(179, 42, 29, 255);
      constexpr ImU32 foreground = IM_COL32(255, 255, 255, 255);
      constexpr ImU32 inactive_foreground = IM_COL32(255, 255, 255, 96);
      constexpr auto minimize_glyph = "\xEE\xA4\xA1";  // U+E921
      constexpr auto maximize_glyph = "\xEE\xA4\xA2";  // U+E922
      constexpr auto restore_glyph = "\xEE\xA4\xA3";  // U+E923
      constexpr auto close_glyph = "\xEE\xA2\xBB";  // U+E8BB
      constexpr ImWchar glyph_ranges[] { 0xE8BB, 0xE8BB, 0xE921, 0xE923, 0 };
      enum class State : std::uint8_t
      {
        inactive,
        normal,
        hovered,
        pressed
      };
    }  // namespace caption

    int resize_border(HWND window)
    {
      const auto dpi = GetDpiForWindow(window);
      return GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
    }

    int caption_height(HWND window)
    {
      const auto dpi = GetDpiForWindow(window);
      return IsZoomed(window) ? MulDiv(caption::maximized_height, static_cast<int>(dpi), caption::reference_dpi)
                              : GetSystemMetricsForDpi(SM_CYCAPTION, dpi) + resize_border(window);
    }

    RECT caption_buttons(HWND window, int width)
    {
      const auto dpi = GetDpiForWindow(window);
      const auto button_width = MulDiv(caption::button_stride, static_cast<int>(dpi), caption::reference_dpi);
      const auto border = GetSystemMetricsForDpi(SM_CYBORDER, dpi);
      const bool maximized = IsZoomed(window);
      return { width - button_width * caption::button_count, maximized ? 0 : border, width,
        caption_height(window) - border };
    }

    bool over_maximize(HWND window, POINT point, int width)
    {
      auto area = caption_buttons(window, width);
      const auto button_width = (area.right - area.left) / caption::button_count;
      area.left += button_width;
      area.right -= button_width;
      return PtInRect(&area, point);
    }

    ImVec4 caption_color(ImGuiID id, caption::State state, ImVec4 target, float delta)
    {
      constexpr ImGuiID start_channels = 1;
      constexpr ImGuiID current_channels = 5;
      constexpr ImGuiID elapsed_key = 9;
      constexpr int uninitialized = -1;
      const std::array values { target.x, target.y, target.z, target.w };
      auto* storage = ImGui::GetStateStorage();
      const auto state_id = static_cast<int>(state);
      const bool initialized = storage->GetInt(id, uninitialized) != uninitialized;
      if (storage->GetInt(id, uninitialized) != state_id)
      {
        for (ImGuiID channel = 0; channel < values.size(); ++channel)
          storage->SetFloat(id + start_channels + channel,
            storage->GetFloat(id + current_channels + channel, values[channel]));
        storage->SetFloat(id + elapsed_key, 0);
        storage->SetInt(id, state_id);
      }
      const float duration = state == caption::State::pressed ? caption::press_seconds
        : state == caption::State::hovered                    ? caption::hover_seconds
                                                              : caption::leave_seconds;
      const float elapsed = storage->GetFloat(id + elapsed_key) + delta;
      storage->SetFloat(id + elapsed_key, elapsed);
      const float progress = initialized ? std::min(elapsed / duration, 1.0F) : 1.0F;
      const float exponent =
        state == caption::State::hovered ? caption::hover_exponent : caption::leave_exponent;
      const float blend = state == caption::State::pressed
        ? progress
        : 1 - std::expm1(exponent * (1 - progress)) / std::expm1(exponent);
      std::array<float, 4> result {};
      for (ImGuiID channel = 0; channel < values.size(); ++channel)
      {
        const auto index = static_cast<int>(channel);
        const float start = storage->GetFloat(id + start_channels + channel);
        result[index] = start + (values[index] - start) * blend;
        storage->SetFloat(id + current_channels + channel, result[index]);
      }
      return { result[0], result[1], result[2], result[3] };
    }

    void clear_backing_surface(HWND window)
    {
      RECT bounds {};
      GetClientRect(window, &bounds);
      const auto dc = GetDC(window);
      PatBlt(dc, 0, 0, bounds.right, bounds.bottom, BLACKNESS);
      ReleaseDC(window, dc);
    }

    // Windows fixes the callback signature and passes NCCALCSIZE_PARAMS through LPARAM.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    LRESULT CALLBACK frame_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR subclass_id,
      DWORD_PTR)
    {
      if ((message == WM_NCLBUTTONDOWN || message == WM_NCLBUTTONDBLCLK) && wparam == HTMAXBUTTON)
      {
        if (SetPropW(window, maximize_pressed, window))
        {
          SetCapture(window);
          if (GetCapture() != window)
            RemovePropW(window, maximize_pressed);
        }
        return 0;
      }
      if (message == WM_LBUTTONUP && GetPropW(window, maximize_pressed))
      {
        RemovePropW(window, maximize_pressed);
        ReleaseCapture();
        RECT client {};
        GetClientRect(window, &client);
        if (over_maximize(window, POINT { GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) }, client.right))
          PostMessageW(window, WM_SYSCOMMAND, IsZoomed(window) ? SC_RESTORE : SC_MAXIMIZE, 0);
        return 0;
      }
      if (message == WM_CAPTURECHANGED || message == WM_CANCELMODE || message == WM_NCDESTROY)
      {
        if (RemovePropW(window, maximize_pressed) && GetCapture() == window)
          ReleaseCapture();
      }
      if (message == WM_ERASEBKGND)
      {
        clear_backing_surface(window);
        return 1;
      }
      if (message == WM_PAINT)
        clear_backing_surface(window);

      if (message == WM_NCCALCSIZE && wparam)
      {
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        auto& bounds = reinterpret_cast<NCCALCSIZE_PARAMS*>(lparam)->rgrc[0];
        const auto top = bounds.top;
        // A positive full-height extension preserves the backdrop without DWM's caption overlay.
        const MARGINS margins { 0, 0, 0, bounds.bottom - bounds.top };
        DwmExtendFrameIntoClientArea(window, &margins);
        DefSubclassProc(window, message, wparam, lparam);
        // Retain the native side/bottom borders and maximized work-area inset.
        bounds.top = top + (IsZoomed(window) ? resize_border(window) : 0);
        return 0;
      }

      LRESULT result {};
      if (message != WM_NCHITTEST && DwmDefWindowProc(window, message, wparam, lparam, &result))
        return result;

      if (message == WM_NCHITTEST)
      {
        POINT point { GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
        ScreenToClient(window, &point);
        RECT client {};
        GetClientRect(window, &client);
        const auto buttons = caption_buttons(window, client.right);
        if (PtInRect(&buttons, point))
          return over_maximize(window, point, client.right) ? HTMAXBUTTON : HTCLIENT;
        result = DefSubclassProc(window, message, wparam, lparam);
        if (result != HTCLIENT)
          return result;
        if (!IsZoomed(window) && point.y < resize_border(window))
        {
          RECT bounds {};
          GetClientRect(window, &bounds);
          const auto dpi = GetDpiForWindow(window);
          const auto corner =
            GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
          if (point.x < corner)
            return HTTOPLEFT;
          if (point.x >= bounds.right - corner)
            return HTTOPRIGHT;
          return HTTOP;
        }
        return point.y < caption_height(window) ? HTCAPTION : HTCLIENT;
      }

      if (message == WM_NCDESTROY)
        RemoveWindowSubclass(window, frame_proc, subclass_id);
      return DefSubclassProc(window, message, wparam, lparam);
    }
  }  // namespace

  void configure_windows_chrome(GLFWwindow* window)
  {
    const auto native = glfwGetWin32Window(window);
    const BOOL dark = TRUE;
    const DWM_SYSTEMBACKDROP_TYPE backdrop = DWMSBT_MAINWINDOW;
    RECT bounds {};
    GetWindowRect(native, &bounds);
    const MARGINS margins { 0, 0, 0, bounds.bottom - bounds.top };
    WTA_OPTIONS caption { WTNCA_NODRAWCAPTION | WTNCA_NODRAWICON | WTNCA_NOSYSMENU,
      WTNCA_NODRAWCAPTION | WTNCA_NODRAWICON | WTNCA_NOSYSMENU };
    if (!SetWindowSubclass(native, frame_proc, 1, 0) || FAILED(SetWindowThemeAttribute(native, WTA_NONCLIENT, &caption, sizeof(caption))) || FAILED(DwmSetWindowAttribute(native, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark))) || FAILED(DwmSetWindowAttribute(native, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop))) || FAILED(DwmExtendFrameIntoClientArea(native, &margins)))
      throw std::runtime_error("Cannot configure Windows Mica Alt backdrop");
    SetWindowPos(native, nullptr, 0, 0, 0, 0,
      SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
  }

  float windows_caption_height(GLFWwindow* window)
  {
    return static_cast<float>(caption_height(glfwGetWin32Window(window)));
  }

  ImFont* load_windows_caption_font()
  {
    std::array<char, MAX_PATH> directory {};
    const auto length = GetWindowsDirectoryA(directory.data(), static_cast<UINT>(directory.size()));
    if (!length || length >= directory.size())
      throw std::runtime_error("Cannot locate Windows fonts directory");
    const auto path = std::string(directory.data()) + "/Fonts/SegoeIcons.ttf";
    ImFontConfig config;
    config.PixelSnapH = true;
    config.OversampleH = 1;
    config.OversampleV = 1;
    auto* font = ImGui::GetIO().Fonts->AddFontFromFileTTF(path.c_str(), caption::icon_size, &config,
      caption::glyph_ranges);
    if (!font)
      throw std::runtime_error("Cannot load system Segoe Fluent Icons font");
    return font;
  }

  void draw_windows_caption(GLFWwindow* window, ImFont* font)
  {
    const auto native = glfwGetWin32Window(window);
    const auto& io = ImGui::GetIO();
    // Use the immutable frame dimensions, including a resize proposal not yet committed by Windows.
    const auto area = caption_buttons(native, static_cast<int>(io.DisplaySize.x));
    const auto button_stride = static_cast<float>(area.right - area.left) / caption::button_count;
    const auto button_height = static_cast<float>(area.bottom - area.top);
    const float scale = static_cast<float>(GetDpiForWindow(native)) / caption::reference_dpi;
    const float gap = std::round(caption::button_gap * scale);
    const float button_width = button_stride - gap;
    ImGui::SetNextWindowPos(ImVec2(static_cast<float>(area.left), static_cast<float>(area.top)));
    ImGui::SetNextWindowSize(ImVec2(button_stride * caption::button_count, button_height));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(0, 0));
    ImGui::Begin("##windows-caption", nullptr,
      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoFocusOnAppearing);
    const bool maximized = IsZoomed(native);
    const bool active = GetForegroundWindow() == native;
    const char* labels[] { "Minimize", maximized ? "Restore" : "Maximize", "Close" };
    const int commands[] { SC_MINIMIZE, maximized ? SC_RESTORE : SC_MAXIMIZE, SC_CLOSE };
    // Windows receives nonclient maximize input so its own Snap Layout tracking stays active.
    POINT cursor {};
    const bool maximize_hover = GetCursorPos(&cursor) && WindowFromPoint(cursor) == native && ScreenToClient(native, &cursor) && over_maximize(native, cursor, static_cast<int>(io.DisplaySize.x));
    auto* draw = ImGui::GetWindowDrawList();
    for (int button = 0; button < caption::button_count; ++button)
    {
      ImGui::SetCursorPos(ImVec2(button_stride * static_cast<float>(button) + gap, 0));
      const auto position = ImGui::GetCursorScreenPos();
      if (ImGui::InvisibleButton(labels[button], ImVec2(button_width, button_height)) && button != caption::maximize_index)
      {
        PostMessageW(native, WM_SYSCOMMAND, static_cast<WPARAM>(commands[button]), 0);
      }
      const bool hovered = button == caption::maximize_index ? maximize_hover : ImGui::IsItemHovered();
      const bool pressed =
        hovered && (button == caption::maximize_index ? GetPropW(native, maximize_pressed) != nullptr : ImGui::IsItemActive());
      const auto state = pressed ? caption::State::pressed
        : hovered                ? caption::State::hovered
        : active                 ? caption::State::normal
                                 : caption::State::inactive;
      const auto target = button == caption::close_index ? (pressed      ? caption::close_pressed_background
                                                               : hovered ? caption::close_hover_background
                                                                         : caption::close_idle_background)
                                                         : (pressed      ? caption::pressed_background
                                                               : hovered ? caption::hover_background
                                                                         : caption::idle_background);
      ImGui::PushID(button);
      const auto background = caption_color(ImGui::GetID("background"), state,
        ImGui::ColorConvertU32ToFloat4(target), io.DeltaTime);
      const auto ink =
        caption_color(ImGui::GetID("foreground"), state,
          ImGui::ColorConvertU32ToFloat4(active || hovered ? caption::foreground
                                                           : caption::inactive_foreground),
          io.DeltaTime);
      ImGui::PopID();
      draw->AddRectFilled(position, ImVec2(position.x + button_width, position.y + button_height),
        ImGui::ColorConvertFloat4ToU32(background));
      const char* glyphs[] { caption::minimize_glyph,
        maximized ? caption::restore_glyph : caption::maximize_glyph,
        caption::close_glyph };
      const float size = caption::icon_size * scale;
      const auto extent = font->CalcTextSizeA(size, FLT_MAX, 0, glyphs[button]);
      const float x = std::floor(position.x + (button_width - extent.x) / 2);
      const float y = std::floor(position.y + (button_height - extent.y) / 2);
      draw->AddText(font, size, ImVec2(x, y), ImGui::ColorConvertFloat4ToU32(ink), glyphs[button]);
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
  }
}  // namespace sranipatch
