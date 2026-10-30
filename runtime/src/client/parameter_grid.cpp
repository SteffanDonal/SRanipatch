#include "parameter_grid.hpp"

#include "sranipatch/lip.hpp"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>

namespace sranipatch
{
  namespace
  {
    struct Slider
    {
      const char* parameter {};
      const char* label {};
    };

    struct Row
    {
      Slider left, centre, right;
    };

    constexpr Row cheek[] {
      { { "cheek_puff_left", "Puff" }, { "cheek_suck", "Suck" }, { "cheek_puff_right", "Puff" } },
    };
    constexpr Row mouth[] {
      { { "mouth_upper_up_left", "Upper Up" },
        { "mouth_upper_overturn", "Overturn" },
        { "mouth_upper_up_right", "Upper Up" } },
      { { "mouth_upper_left", "Upper" }, { "mouth_upper_inside", "Inside" }, { "mouth_upper_right", "Upper" } },
      { { "mouth_smile_left", "Smile" }, { "mouth_pout", "Pout" }, { "mouth_smile_right", "Smile" } },
      { { "mouth_sad_left", "Sad" }, { "mouth_lower_inside", "Inside" }, { "mouth_sad_right", "Sad" } },
      { { "mouth_lower_left", "Lower" }, { "mouth_lower_overlay", "Overlay" }, { "mouth_lower_right", "Lower" } },
      { { "mouth_lower_down_left", "Lower Down" },
        { "mouth_lower_overturn", "Overturn" },
        { "mouth_lower_down_right", "Lower Down" } },
      { {}, { "mouth_ape_shape", "Ape Shape" }, {} },
    };
    constexpr Row jaw[] {
      { { "jaw_left", "Left" }, { "jaw_open", "Open" }, { "jaw_right", "Right" } },
      { {}, { "jaw_forward", "Forward" }, {} },
    };
    constexpr Row tongue[] {
      { { "tongue_up_left_morph", "Morph" }, { "tongue_up", "Up" }, { "tongue_up_right_morph", "Morph" } },
      { { "tongue_left", "Left" }, { "tongue_roll", "Roll" }, { "tongue_right", "Right" } },
      { { "tongue_down_left_morph", "Morph" }, { "tongue_down", "Down" }, { "tongue_down_right_morph", "Morph" } },
      { {}, { "tongue_long_step_1", "Long 1" }, {} },
      { {}, { "tongue_long_step_2", "Long 2" }, {} },
    };

    struct Section
    {
      const char* label;
      std::span<const Row> rows;
    };

    constexpr Section sections[] {
      { "Cheek", cheek },
      { "Mouth", mouth },
      { "Jaw", jaw },
      { "Tongue", tongue },
    };

    static_assert(
      [] {
        std::array<bool, lip_parameter_count> seen {};
        for (const auto& section : sections)
          for (const auto& row : section.rows)
            for (const auto& slider : { row.left, row.centre, row.right })
            {
              if (!slider.parameter)
                continue;
              const auto found = std::find(lip_parameter_names.begin(), lip_parameter_names.end(),
                std::string_view(slider.parameter));
              if (found == lip_parameter_names.end())
                return false;
              const auto index = static_cast<std::size_t>(found - lip_parameter_names.begin());
              if (seen[index])
                return false;
              seen[index] = true;
            }
        return std::all_of(seen.begin(), seen.end(), [](bool present) { return present; });
      }(),
      "The grid must include every lip parameter exactly once"
    );

    enum class Origin : std::uint8_t
    {
      right,
      centre,
      left
    };

    void draw_slider(const Slider& slider, std::span<const Parameter> parameters, Origin origin)
    {
      if (!slider.parameter)
        return;
      const auto found = std::find_if(parameters.begin(), parameters.end(), [&](const Parameter& parameter) {
        return std::string_view(parameter.name.data()) == slider.parameter;
      });
      if (found == parameters.end())
        return;
      const float value = std::clamp(found->value, 0.0F, 1.0F);
      const auto& style = ImGui::GetStyle();
      const float width = std::max(1.0F, ImGui::GetContentRegionAvail().x);
      ImGui::Dummy(ImVec2(width, ImGui::GetFrameHeight()));
      const auto min = ImGui::GetItemRectMin();
      const auto max = ImGui::GetItemRectMax();
      auto* draw_list = ImGui::GetWindowDrawList();
      ImGui::BeginDisabled();
      draw_list->AddRectFilled(min, max, ImGui::GetColorU32(ImGuiCol_FrameBg), style.FrameRounding);
      float fill_left = min.x;
      float fill_right = max.x;
      if (origin == Origin::right)
        fill_left = max.x - width * value;
      else if (origin == Origin::left)
        fill_right = min.x + width * value;
      else
      {
        const float centre = (min.x + max.x) * 0.5F;
        fill_left = centre - width * value * 0.5F;
        fill_right = centre + width * value * 0.5F;
      }
      if (value > 0.0F)
        draw_list->AddRectFilled(ImVec2(fill_left, min.y), ImVec2(fill_right, max.y),
          ImGui::GetColorU32(ImGuiCol_SliderGrab), style.FrameRounding);
      if (style.FrameBorderSize > 0.0F)
        draw_list->AddRect(min, max, ImGui::GetColorU32(ImGuiCol_Border), style.FrameRounding,
          ImDrawFlags_None, style.FrameBorderSize);
      ImGui::EndDisabled();
      const auto size = ImGui::CalcTextSize(slider.label);
      float text_x = min.x + style.FramePadding.x;
      if (origin == Origin::right)
        text_x = std::max(text_x, max.x - style.FramePadding.x - size.x);
      else if (origin == Origin::centre)
        text_x = std::max(text_x, (min.x + max.x - size.x) * 0.5F);
      const ImVec4 clip(min.x, min.y, max.x, max.y);
      draw_list->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
        ImVec2(text_x, min.y + (max.y - min.y - size.y) * 0.5F),
        ImGui::GetColorU32(ImGuiCol_Text), slider.label, nullptr, 0.0F, &clip);
    }
  }  // namespace

  void draw_parameter_grid(std::span<const Parameter> parameters)
  {
    if (parameters.empty())
      return;
    const float label_width = ImGui::CalcTextSize("Tongue").x;
    bool first_section = true;
    for (const auto& section : sections)
    {
      if (!first_section)
        ImGui::Separator();
      first_section = false;
      if (!ImGui::BeginTable(section.label, 4, ImGuiTableFlags_SizingStretchSame))
        continue;
      ImGui::TableSetupColumn("Section", ImGuiTableColumnFlags_WidthFixed, label_width);
      ImGui::TableSetupColumn("Left", ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("Centre", ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("Right", ImGuiTableColumnFlags_WidthStretch);
      ImVec2 label_min {}, label_max {};
      for (std::size_t i = 0; i < section.rows.size(); ++i)
      {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        if (i == 0)
          label_min = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(label_width, ImGui::GetFrameHeight()));
        label_max = ImGui::GetItemRectMax();
        const auto& row = section.rows[i];
        ImGui::TableSetColumnIndex(1);
        draw_slider(row.left, parameters, Origin::right);
        ImGui::TableSetColumnIndex(2);
        draw_slider(row.centre, parameters, Origin::centre);
        ImGui::TableSetColumnIndex(3);
        draw_slider(row.right, parameters, Origin::left);
      }
      ImGui::TableSetColumnIndex(0);
      const auto text_size = ImGui::CalcTextSize(section.label);
      ImGui::GetWindowDrawList()->AddText(
        ImVec2(label_min.x, (label_min.y + label_max.y - text_size.y) * 0.5F),
        ImGui::GetColorU32(ImGuiCol_Text), section.label
      );
      ImGui::EndTable();
    }
  }
}
