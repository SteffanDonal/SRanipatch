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

    colors[ImGuiCol_WindowBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_ChildBg] = ImVec4(0.14f, 0.15f, 0.17f, 1.00f);
    colors[ImGuiCol_PopupBg] = ImVec4(0.10f, 0.10f, 0.12f, 0.95f);
    colors[ImGuiCol_Border] = ImVec4(0.30f, 0.33f, 0.42f, 0.40f);

    colors[ImGuiCol_Text] = ImVec4(0.90f, 0.93f, 0.95f, 1.00f);
    colors[ImGuiCol_TextDisabled] = ImVec4(0.60f, 0.65f, 0.70f, 1.00f);

    colors[ImGuiCol_Header] = ImVec4(0.36f, 0.42f, 0.55f, 0.60f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.44f, 0.50f, 0.68f, 0.80f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.46f, 0.55f, 0.75f, 1.00f);

    colors[ImGuiCol_Button] = ImVec4(0.28f, 0.34f, 0.48f, 0.70f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.36f, 0.45f, 0.65f, 0.85f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.40f, 0.50f, 0.70f, 1.00f);

    colors[ImGuiCol_FrameBg] = ImVec4(0.20f, 0.22f, 0.28f, 1.00f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.28f, 0.32f, 0.42f, 1.00f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.32f, 0.38f, 0.50f, 1.00f);

    colors[ImGuiCol_Tab] = ImVec4(0.26f, 0.30f, 0.42f, 0.80f);
    colors[ImGuiCol_TabHovered] = ImVec4(0.36f, 0.42f, 0.58f, 1.00f);
    colors[ImGuiCol_TabSelected] = ImVec4(0.42f, 0.50f, 0.68f, 1.00f);
    colors[ImGuiCol_TabDimmed] = ImVec4(0.20f, 0.24f, 0.32f, 0.80f);
    colors[ImGuiCol_TabDimmedSelected] = ImVec4(0.30f, 0.36f, 0.50f, 1.00f);

    colors[ImGuiCol_TitleBg] = ImVec4(0.20f, 0.25f, 0.30f, 1.00f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.25f, 0.30f, 0.40f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.10f, 0.12f, 0.15f, 0.75f);

    colors[ImGuiCol_ScrollbarBg] = ImVec4(0.13f, 0.14f, 0.18f, 1.00f);
    colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.25f, 0.30f, 0.38f, 0.60f);
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.35f, 0.40f, 0.50f, 0.80f);
    colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.45f, 0.50f, 0.65f, 1.00f);

    colors[ImGuiCol_CheckMark] = ImVec4(0.80f, 0.85f, 1.00f, 1.00f);
    colors[ImGuiCol_SliderGrab] = ImVec4(0.50f, 0.65f, 0.90f, 1.00f);
    colors[ImGuiCol_SliderGrabActive] = ImVec4(0.60f, 0.75f, 1.00f, 1.00f);

    colors[ImGuiCol_ResizeGrip] = ImVec4(0.30f, 0.40f, 0.50f, 0.60f);
    colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.40f, 0.50f, 0.60f, 0.80f);
    colors[ImGuiCol_ResizeGripActive] = ImVec4(0.50f, 0.60f, 0.80f, 1.00f);

    colors[ImGuiCol_Separator] = ImVec4(0.35f, 0.40f, 0.48f, 0.70f);
    colors[ImGuiCol_SeparatorHovered] = ImVec4(0.50f, 0.60f, 0.72f, 0.90f);
    colors[ImGuiCol_SeparatorActive] = ImVec4(0.65f, 0.70f, 0.85f, 1.00f);

    colors[ImGuiCol_MenuBarBg] = ImVec4(0.14f, 0.15f, 0.17f, 1.00f);
    colors[ImGuiCol_DragDropTarget] = ImVec4(0.50f, 0.85f, 1.00f, 0.90f);

    style.WindowRounding = 8.0f;
    style.ChildRounding = 6.0f;
    style.FrameRounding = 5.0f;
    style.PopupRounding = 6.0f;
    style.GrabRounding = 4.0f;
    style.TabRounding = 5.0f;

    style.ScrollbarPadding = 3.0f;
    style.ScrollbarRounding = 3.0f;
    style.ScrollbarSize = 20.0f;

    style.WindowBorderSize = 0.0f;
    style.FrameBorderSize = 0.0f;
    style.PopupBorderSize = 1.0f;

    style.WindowPadding = ImVec2(16, 16);
    style.FramePadding = ImVec2(10, 6);
    style.ItemSpacing = ImVec2(10, 10);
    style.ItemInnerSpacing = ImVec2(6, 4);
    style.IndentSpacing = 20.0f;

    ImPlot::StyleColorsAuto();
    auto& plot_style = ImPlot::GetStyle();
    plot_style.Colormap = ImPlotColormap_Paired;
    plot_style.PlotPadding = { 0, 0 };
  }
}  // namespace sranipatch
