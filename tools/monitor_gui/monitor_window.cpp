#include "monitor_window.hpp"
#include "st/value_text.hpp"
#include "util/text.hpp"
#include <algorithm>
#include <imgui.h>
#include <imgui_stdlib.h>
#include <implot.h>
#include <optional>
#include <vector>

namespace softplc::monitor {

namespace {

constexpr double kTrendSeconds = 30.0;

const ImVec4 kGreen{0.35F, 0.85F, 0.45F, 1.0F};
const ImVec4 kRed{0.95F, 0.35F, 0.35F, 1.0F};
const ImVec4 kGrey{0.6F, 0.6F, 0.6F, 1.0F};
const ImVec4 kOrange{1.0F, 0.65F, 0.2F, 1.0F};

st::DataType typeFromName(const std::string& name) {
    if (name == "BOOL") return st::DataType::Bool;
    if (name == "INT") return st::DataType::Int;
    if (name == "DINT") return st::DataType::DInt;
    if (name == "REAL") return st::DataType::Real;
    if (name == "TIME") return st::DataType::Time;
    return st::DataType::Unknown;
}

// A value as a number for plotting: BOOL as 0/1, TIME in milliseconds.
std::optional<double> plottable(const VariableRow& row) {
    const auto value = st::parseValue(row.value, typeFromName(row.type));
    if (!value) {
        return std::nullopt;
    }
    if (const auto* b = std::get_if<bool>(&*value)) return *b ? 1.0 : 0.0;
    if (const auto* i = std::get_if<std::int64_t>(&*value)) return static_cast<double>(*i);
    return std::get<double>(*value);
}

} // namespace

MonitorWindow::MonitorWindow(Poller& poller, std::string host, std::uint16_t port)
    : poller_(poller), host_(std::move(host)), port_(port) {}

void MonitorWindow::draw() {
    const auto snapshot = poller_.snapshot();
    recordHistory(snapshot);

    const auto* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin("soft_plc monitor", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);

    drawConnectionBar(snapshot);
    drawStatus(snapshot);
    ImGui::Separator();

    const float trendHeight = plotted_.empty() ? 0.0F : ImGui::GetContentRegionAvail().y * 0.4F;
    ImGui::BeginChild("variables", ImVec2(0, -trendHeight - (plotted_.empty() ? 0.0F : 8.0F)));
    drawVariableTable(snapshot);
    ImGui::EndChild();

    if (!plotted_.empty()) {
        drawTrend();
    }
    ImGui::End();
}

void MonitorWindow::drawConnectionBar(const Snapshot& snapshot) {
    ImGui::SetNextItemWidth(180);
    ImGui::InputText("Host", &host_);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    ImGui::InputInt("Port", &port_);
    port_ = std::clamp(port_, 1, 65535);
    ImGui::SameLine();
    if (ImGui::Button("Connect")) {
        poller_.connect(host_, static_cast<std::uint16_t>(port_));
    }
    ImGui::SameLine();
    if (ImGui::Button("Disconnect")) {
        poller_.disconnect();
    }
    ImGui::SameLine();
    if (snapshot.connected) {
        ImGui::TextColored(kGreen, "connected");
    } else {
        ImGui::TextColored(kGrey, "%s", snapshot.connectionError.empty() ? "not connected"
                                                                         : snapshot.connectionError.c_str());
    }
}

void MonitorWindow::drawStatus(const Snapshot& snapshot) {
    if (!snapshot.connected) {
        return;
    }
    const bool running = snapshot.state == "RUN";
    ImGui::TextColored(running ? kGreen : kRed, "%s", snapshot.state.c_str());
    ImGui::SameLine();
    ImGui::TextUnformatted(snapshot.statistics.c_str());
    if (!snapshot.stopError.empty()) {
        ImGui::TextColored(kRed, "Runtime error: %s", snapshot.stopError.c_str());
    }
    if (!snapshot.lastReply.empty()) {
        ImGui::TextColored(kGrey, "Last command: %s", snapshot.lastReply.c_str());
    }
}

void MonitorWindow::drawVariableTable(const Snapshot& snapshot) {
    ImGui::SetNextItemWidth(300);
    ImGui::InputTextWithHint("##filter", "filter, e.g. timer or fast.", &filter_);
    const auto filter = toUpper(filter_);

    constexpr auto flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                           ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("table", 6, flags)) {
        return;
    }
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Plot", ImGuiTableColumnFlags_WidthFixed, 36);
    ImGui::TableSetupColumn("Variable", ImGuiTableColumnFlags_WidthStretch, 3.0F);
    ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 50);
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 1.5F);
    ImGui::TableSetupColumn("New value", ImGuiTableColumnFlags_WidthFixed, 110);
    ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 230);
    ImGui::TableHeadersRow();

    for (const auto& row : snapshot.variables) {
        if (!filter.empty() && toUpper(row.path).find(filter) == std::string::npos) {
            continue;
        }
        ImGui::PushID(row.path.c_str()); // the same widget labels repeat on every row
        ImGui::TableNextRow();

        ImGui::TableNextColumn();
        bool plot = plotted_.count(row.path) > 0;
        if (ImGui::Checkbox("##plot", &plot)) {
            if (plot) {
                plotted_.insert(row.path);
            } else {
                plotted_.erase(row.path);
                history_.erase(row.path);
            }
        }

        ImGui::TableNextColumn();
        ImGui::TextUnformatted(row.path.c_str());
        ImGui::TableNextColumn();
        ImGui::TextColored(kGrey, "%s", row.type.c_str());
        ImGui::TableNextColumn();
        if (row.forced) {
            ImGui::TextColored(kOrange, "%s  F", row.value.c_str());
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("forced");
            }
        } else {
            ImGui::TextUnformatted(row.value.c_str());
        }

        ImGui::TableNextColumn();
        auto& edit = edits_[row.path];
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##edit", row.type == "TIME" ? "T#2s" : row.type == "BOOL" ? "TRUE" : "value", &edit);

        ImGui::TableNextColumn();
        const bool haveEdit = !edit.empty();
        if (row.type == "BOOL") {
            if (ImGui::SmallButton("Toggle")) {
                poller_.send("WRITE " + row.path + (row.value == "TRUE" ? " FALSE" : " TRUE"));
            }
            ImGui::SameLine();
        }
        ImGui::BeginDisabled(!haveEdit);
        if (ImGui::SmallButton("Write")) {
            poller_.send("WRITE " + row.path + " " + edit);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Force")) {
            poller_.send("FORCE " + row.path + " " + edit);
        }
        ImGui::EndDisabled();
        if (row.forced) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Unforce")) {
                poller_.send("UNFORCE " + row.path);
            }
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void MonitorWindow::recordHistory(const Snapshot& snapshot) {
    if (!snapshot.connected || snapshot.sequence == lastSequence_) {
        return; // no new data since the last frame
    }
    lastSequence_ = snapshot.sequence;
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
    for (const auto& row : snapshot.variables) {
        if (plotted_.count(row.path) == 0) {
            continue;
        }
        if (const auto value = plottable(row)) {
            auto& series = history_[row.path];
            series.isBool = row.type == "BOOL";
            auto& points = series.points;
            points.emplace_back(now, *value);
            while (!points.empty() && points.front().first < now - kTrendSeconds) {
                points.pop_front();
            }
        }
    }
}

void MonitorWindow::drawTrend() {
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
    if (!ImPlot::BeginPlot("Trend", ImVec2(-1, -1))) {
        return;
    }
    // X: seconds ago (-30 .. 0). Y1: numbers, auto-scaled. Y2: BOOLs, fixed 0/1.
    ImPlot::SetupAxes("seconds ago", "value", ImPlotAxisFlags_None, ImPlotAxisFlags_AutoFit);
    ImPlot::SetupAxisLimits(ImAxis_X1, -kTrendSeconds, 0.0, ImPlotCond_Always);
    ImPlot::SetupAxis(ImAxis_Y2, "BOOL", ImPlotAxisFlags_AuxDefault);
    ImPlot::SetupAxisLimits(ImAxis_Y2, -0.1, 1.1, ImPlotCond_Always);
    for (const auto& [path, series] : history_) {
        std::vector<double> xs;
        std::vector<double> ys;
        xs.reserve(series.points.size());
        ys.reserve(series.points.size());
        for (const auto& [x, y] : series.points) {
            xs.push_back(x - now);
            ys.push_back(y);
        }
        ImPlot::SetAxes(ImAxis_X1, series.isBool ? ImAxis_Y2 : ImAxis_Y1);
        ImPlot::PlotStairs(path.c_str(), xs.data(), ys.data(), static_cast<int>(xs.size()));
    }
    ImPlot::EndPlot();
}

} // namespace softplc::monitor
