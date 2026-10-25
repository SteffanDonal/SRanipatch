#include "performance_plot.hpp"

#include <imgui.h>
#include <imgui_internal.h>
#include <implot.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <limits>

namespace sranipatch
{
  namespace
  {
    constexpr double history_seconds = 30.0;
    constexpr double stale_seconds = 0.25;
    constexpr double target_fps = 60.0;
    constexpr double target_ms = 1000.0 / target_fps;

    void reference(double value, const char* label, ImAxis axis)
    {
      const auto colour = axis == ImAxis_Y1 ? IM_COL32(130, 190, 230, 180) : IM_COL32(240, 190, 140, 180);
      ImPlot::SetAxes(ImAxis_X1, axis);
      const auto limits = ImPlot::GetPlotLimits();
      const auto left = ImPlot::PlotToPixels(limits.X.Min, value);
      const auto right = ImPlot::PlotToPixels(limits.X.Max, value);
      ImPlot::PushPlotClipRect();
      auto* draw = ImPlot::GetPlotDrawList();
      const int dashes = static_cast<int>(std::ceil((right.x - left.x) / 10.0F));
      for (int i = 0; i < dashes; ++i)
      {
        const float x = left.x + static_cast<float>(i) * 10.0F;
        draw->AddLine(ImVec2(x, left.y), ImVec2(std::min(x + 5.0F, right.x), left.y), colour);
      }
      const float label_x = axis == ImAxis_Y1 ? left.x + 4.0F : right.x - ImGui::CalcTextSize(label).x - 4.0F;
      draw->AddText(ImVec2(label_x, left.y - ImGui::GetTextLineHeight()), colour, label);
      ImPlot::PopPlotClipRect();
    }
  }  // namespace

  void PerformancePlot::update(const Status& status, bool connected, std::uint64_t now)
  {
    if (connected && session_ != status.session_id)
    {
      session_ = status.session_id;
      started_at_ = now;
      capture_ = {};
      inference_ = {};
    }
    if (!started_at_)
      started_at_ = now;
    elapsed_ = static_cast<double>(now - started_at_) / 1e9;
    const bool running = connected && status.state == ServerState::running;
    const auto update_series = [&](Series& series, std::uint64_t counter, std::array<double, 3> values) {
      if (running && counter != series.counter)
      {
        series.samples.push_back({ elapsed_, values });
        series.gap = false;
      }
      else if (!series.gap && (!running || elapsed_ - series.samples.back().time > stale_seconds))
      {
        constexpr auto missing = std::numeric_limits<double>::quiet_NaN();
        series.samples.push_back({ elapsed_, { missing, missing, missing } });
        series.gap = true;
      }
      series.counter = counter;
      while (!series.samples.empty() && series.samples.front().time < elapsed_ - history_seconds)
        series.samples.pop_front();
    };
    update_series(capture_, status.frames_captured, { status.capture_fps, 0, 0 });
    update_series(inference_, status.frames_inferred,
      { status.inference_fps, status.inference_ms, status.frame_age_ms });
  }

  void PerformancePlot::draw(const Status& status) const
  {
    struct Metric
    {
      const char* name;
      const char* id;
      const char* unit;
      const Series* series;
      std::size_t value;
      ImAxis axis;
      ImVec4 colour;
    };

    const std::array metrics {
      Metric { "Capture", "capture", "fps", &capture_, 0, ImAxis_Y1, { 0.50F, 0.75F, 1.0F, 1.0F } },
      Metric { "Inference", "inference-fps", "fps", &inference_, 0, ImAxis_Y1, { 0.55F, 0.90F, 0.70F, 1.0F } },
      Metric { "Frame age", "frame-age", "ms", &inference_, 2, ImAxis_Y2, { 0.85F, 0.65F, 1.0F, 1.0F } },
      Metric { "Inference", "inference-ms", "ms", &inference_, 1, ImAxis_Y2, { 1.0F, 0.75F, 0.50F, 1.0F } }
    };
    const float plot_right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;

    if (ImPlot::BeginPlot("##performance", ImVec2(-1, 200),
          ImPlotFlags_CanvasOnly | ImPlotFlags_Crosshairs | ImPlotFlags_NoFrame))
    {
      constexpr auto axis_flags = ImPlotAxisFlags_Lock | ImPlotAxisFlags_NoMenus | ImPlotAxisFlags_NoSideSwitch | ImPlotAxisFlags_NoHighlight | ImPlotAxisFlags_NoTickMarks;

      ImPlot::SetupAxis(ImAxis_X1, nullptr, axis_flags | ImPlotAxisFlags_NoTickLabels);
      ImPlot::SetupAxisLimits(ImAxis_X1, elapsed_ - history_seconds, elapsed_, ImPlotCond_Always);

      ImPlot::SetupAxis(ImAxis_Y1, nullptr, axis_flags);
      ImPlot::SetupAxisLimits(ImAxis_Y1, 0, target_fps * 1.15, ImPlotCond_Always);

      ImPlot::SetupAxis(ImAxis_Y2, nullptr, axis_flags | ImPlotAxisFlags_AuxDefault);
      ImPlot::SetupAxisLimits(ImAxis_Y2, 0, target_ms * 1.15, ImPlotCond_Always);

      for (const auto& metric : metrics)
      {
        ImPlot::SetAxes(ImAxis_X1, metric.axis);
        ImPlotSpec spec;
        spec.LineColor = metric.colour;
        spec.LineWeight = 1.0F;
        ImPlot::PlotLineG(
          metric.id,
          [](int index, void* data) {
            const auto& item = *static_cast<const Metric*>(data);
            const auto& sample = item.series->samples[static_cast<std::size_t>(index)];
            return ImPlotPoint(sample.time, sample.values[item.value]);
          },
          const_cast<Metric*>(&metric), static_cast<int>(metric.series->samples.size()), spec
        );
      }
      reference(target_fps, "60 fps", ImAxis_Y1);
      reference(target_ms, "16.67 ms", ImAxis_Y2);

      const auto plot_position = ImPlot::GetPlotPos();
      const auto plot_size = ImPlot::GetPlotSize();
      const ImVec2 plot_end(plot_position.x + plot_size.x, plot_position.y + plot_size.y);

      if (ImPlot::IsPlotHovered())
      {
        const double time = ImPlot::GetPlotMousePos().x;
        ImGui::SetNextWindowPos(
          ImVec2(plot_right + ImGui::GetStyle().ItemSpacing.x, plot_position.y + plot_size.y * 0.5F),
          ImGuiCond_Always, ImVec2(0, 0.5F)
        );

        ImGui::BeginTooltip();

        ImGui::Text("%.2f s ago", elapsed_ - time);

        std::array<std::array<char, 64>, metrics.size()> values {};
        for (std::size_t i = 0; i < metrics.size(); ++i)
        {
          const auto& metric = metrics[i];
          const auto& samples = metric.series->samples;
          const auto next =
            std::lower_bound(samples.begin(), samples.end(), time,
              [](const Sample& sample, double x) { return sample.time < x; });
          const Sample* nearest = next == samples.end() ? nullptr : &*next;
          if (next != samples.begin())
          {
            const auto& previous = *std::prev(next);
            if (!nearest || time - previous.time < nearest->time - time)
              nearest = &previous;
          }
          if (nearest && std::abs(nearest->time - time) <= stale_seconds && std::isfinite(nearest->values[metric.value]))
            std::snprintf(values[i].data(), values[i].size(), "%.2f", nearest->values[metric.value]);
          else
            std::snprintf(values[i].data(), values[i].size(), "--");
        }

        if (ImGui::BeginTable("hover-metrics", 3))
        {
          ImGui::TableSetupColumn("Metric", ImGuiTableColumnFlags_WidthStretch);
          ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, 45);
          ImGui::TableSetupColumn("Unit", ImGuiTableColumnFlags_WidthFixed);

          ImAxis previous_axis = IMPLOT_AUTO;
          for (std::size_t i = 0; i < metrics.size(); ++i)
          {
            const auto& metric = metrics[i];
            if (metric.axis != previous_axis)
            {
              ImGui::TableNextRow();
              ImGui::TableSetColumnIndex(0);
              ImGui::NewLine();
              previous_axis = metric.axis;
            }

            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            ImGui::TextColored(metric.colour, "%s", metric.name);

            ImGui::TableSetColumnIndex(1);
            ImGui::PushStyleColor(ImGuiCol_Text, metric.colour);
            ImGui::TextAligned(1.0f, ImGui::GetColumnWidth(), "%s", values[i].data());
            ImGui::PopStyleColor(1);

            ImGui::TableSetColumnIndex(2);
            ImGui::TextColored(metric.colour, "%s", metric.unit);
          }
          ImGui::EndTable();
        }

        ImGui::EndTooltip();
      }
      ImPlot::EndPlot();
    }

    ImGui::Indent(26);
    if (ImGui::BeginTable("frame-counters", 3, ImGuiTableFlags_SizingStretchSame))
    {
      ImGui::TableSetupColumn("Captured", ImGuiTableColumnFlags_WidthStretch, 3);
      ImGui::TableSetupColumn("Inferred", ImGuiTableColumnFlags_WidthStretch, 3);
      ImGui::TableSetupColumn("Skipped", ImGuiTableColumnFlags_WidthStretch, 2);

      ImGui::TableNextColumn();
      ImGui::Text("%s: %llu", "Captured", static_cast<unsigned long long>(status.frames_captured));

      ImGui::TableNextColumn();
      ImGui::Text("%s: %llu", "Inferred", static_cast<unsigned long long>(status.frames_inferred));

      ImGui::TableNextColumn();
      ImGui::Text("%s: %llu", "Skipped", static_cast<unsigned long long>(status.frames_skipped));

      ImGui::EndTable();
    }
    ImGui::Unindent();
  }
}  // namespace sranipatch
