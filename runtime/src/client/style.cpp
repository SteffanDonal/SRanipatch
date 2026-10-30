#include "style.hpp"

#include <imgui.h>
#include <implot.h>

namespace sranipatch
{
  void set_dark_style()
  {
    ImGui::StyleColorsDark();
    auto& style = ImGui::GetStyle();
    auto* colors = style.Colors;

    const auto to_normal = 1.0f / 255.0f;

    auto transparent = ImVec4(1, 1, 1, 0);
    auto white = ImVec4(1, 1, 1, 1);
    auto black = ImVec4(0, 0, 0, 1);
    auto accent = ImVec4(108 * to_normal, 69 * to_normal, 114 * to_normal, 1);
    auto accent_light = ImVec4(241 * to_normal, 154 * to_normal, 255 * to_normal, 1);

    auto smoke = ImVec4(0, 0, 0, 0x4D * to_normal);

    auto layer_base =
#ifdef _WIN32
      transparent;
#else
      ImVec4(0x20 * to_normal, 0x20 * to_normal, 0x20 * to_normal, 1);
#endif

    auto layer_main = ImVec4(1, 1, 1, 0x09 * to_normal);
    auto layer_internal = ImVec4(0x3A * to_normal, 0x3A * to_normal, 0x3A * to_normal, 0x4C * to_normal);

    auto card_fill = ImVec4(1, 1, 1, 0x0D * to_normal);
    auto card_fill_hover = ImVec4(1, 1, 1, 0x12 * to_normal);
    auto solid_fill_popup = ImVec4(0x2C * to_normal, 0x2C * to_normal, 0x2C * to_normal, 1);

    auto text_disabled = ImVec4(1, 1, 1, 0x5D * to_normal);

    auto control_fill = ImVec4(1, 1, 1, 0x0F * to_normal);
    auto control_fill_hover = ImVec4(1, 1, 1, 0x15 * to_normal);
    auto control_fill_press = ImVec4(1, 1, 1, 0x08 * to_normal);
    auto control_fill_input_active = ImVec4(0x1E * to_normal, 0x1E * to_normal, 0x1E * to_normal, 0xB3 * to_normal);

    auto control_strong_fill = ImVec4(1, 1, 1, 0x8B * to_normal);

    auto subtle_fill = transparent;
    auto subtle_fill_hover = control_fill;
    auto subtle_fill_press = ImVec4(1, 1, 1, 0x0A * to_normal);

    auto popup_stroke = ImVec4(0x75 * to_normal, 0x75 * to_normal, 0x75 * to_normal, 0x34 * to_normal);
    auto divider_stroke = control_fill_hover;

    colors[ImGuiCol_Text] = white;
    colors[ImGuiCol_TextDisabled] = text_disabled;
    colors[ImGuiCol_WindowBg] = layer_base;
    colors[ImGuiCol_ChildBg] = layer_main;
    colors[ImGuiCol_PopupBg] = solid_fill_popup;
    colors[ImGuiCol_Border] = popup_stroke;
    colors[ImGuiCol_BorderShadow] = {};
    colors[ImGuiCol_FrameBg] = control_fill;
    colors[ImGuiCol_FrameBgHovered] = control_fill_hover;
    colors[ImGuiCol_FrameBgActive] = control_fill_input_active;
    colors[ImGuiCol_TitleBg] = layer_internal;
    colors[ImGuiCol_TitleBgActive] = layer_internal;
    colors[ImGuiCol_TitleBgCollapsed] = layer_internal;
    colors[ImGuiCol_MenuBarBg] = transparent;
    colors[ImGuiCol_ScrollbarBg] = solid_fill_popup;
    colors[ImGuiCol_ScrollbarGrab] = control_strong_fill;
    colors[ImGuiCol_ScrollbarGrabHovered] = control_strong_fill;
    colors[ImGuiCol_ScrollbarGrabActive] = control_strong_fill;
    colors[ImGuiCol_CheckMark] = black;
    colors[ImGuiCol_SliderGrab] = accent_light;
    colors[ImGuiCol_SliderGrabActive] = accent_light;
    colors[ImGuiCol_Button] = control_fill;
    colors[ImGuiCol_ButtonHovered] = control_fill_hover;
    colors[ImGuiCol_ButtonActive] = control_fill_press;
    colors[ImGuiCol_Header] = card_fill;
    colors[ImGuiCol_HeaderHovered] = card_fill_hover;
    colors[ImGuiCol_HeaderActive] = card_fill_hover;
    colors[ImGuiCol_Separator] = divider_stroke;
    colors[ImGuiCol_SeparatorHovered] = accent;
    colors[ImGuiCol_SeparatorActive] = accent;
    colors[ImGuiCol_ResizeGrip] = control_strong_fill;
    colors[ImGuiCol_ResizeGripHovered] = control_strong_fill;
    colors[ImGuiCol_ResizeGripActive] = control_strong_fill;
    colors[ImGuiCol_InputTextCursor] = white;
    colors[ImGuiCol_TabHovered] = subtle_fill_hover;
    colors[ImGuiCol_Tab] = subtle_fill;
    colors[ImGuiCol_TabSelected] = subtle_fill_press;
    colors[ImGuiCol_TabSelectedOverline] = accent_light;
    colors[ImGuiCol_TabDimmed] = transparent;
    colors[ImGuiCol_TabDimmedSelected] = control_fill_press;
    colors[ImGuiCol_TabDimmedSelectedOverline] = accent;
    colors[ImGuiCol_TableHeaderBg] = control_fill;
    colors[ImGuiCol_TableBorderStrong] = {};
    colors[ImGuiCol_TableBorderLight] = {};
    colors[ImGuiCol_TableRowBg] = {};
    colors[ImGuiCol_TableRowBgAlt] = {};
    colors[ImGuiCol_TextLink] = accent_light;
    colors[ImGuiCol_TextSelectedBg] = accent;
    colors[ImGuiCol_TreeLines] = {};
    colors[ImGuiCol_DragDropTarget] = accent_light;
    colors[ImGuiCol_NavWindowingDimBg] = smoke;
    colors[ImGuiCol_ModalWindowDimBg] = smoke;

    style.WindowRounding = 8;
    style.ChildRounding = 8;
    style.FrameRounding = 4;
    style.PopupRounding = 8;
    style.GrabRounding = 4;
    style.TabRounding = 4;

    style.ScrollbarPadding = 4;
    style.ScrollbarRounding = 4;
    style.ScrollbarSize = 20;

    style.WindowBorderSize = 0;
    style.FrameBorderSize = 1;
    style.PopupBorderSize = 1;

    style.WindowPadding = ImVec2(16, 16);
    style.FramePadding = ImVec2(8, 8);
    style.ItemSpacing = ImVec2(8, 8);
    style.ItemInnerSpacing = ImVec2(8, 8);

    style.IndentSpacing = 20;

    ImPlot::StyleColorsAuto();
    auto& plot_style = ImPlot::GetStyle();
    plot_style.Colormap = ImPlotColormap_Paired;
    plot_style.PlotPadding = { 0, 0 };
  }
}
