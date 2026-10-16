#pragma once
#include "poller.hpp"
#include <chrono>
#include <deque>
#include <map>
#include <set>
#include <string>

namespace softplc::monitor {

// The monitor's user interface, drawn with Dear ImGui once per frame:
// a connection bar, the PLC status, a filterable table of every variable with
// write/force controls, and a trend plot of the variables ticked for plotting.
class MonitorWindow {
public:
    MonitorWindow(Poller& poller, std::string host, std::uint16_t port);

    void draw();

    // Adds a variable to the trend plot.
    void plot(const std::string& path) { plotted_.insert(path); }

private:
    void drawConnectionBar(const Snapshot& snapshot);
    void drawStatus(const Snapshot& snapshot);
    void drawVariableTable(const Snapshot& snapshot);
    void drawTrend();
    void recordHistory(const Snapshot& snapshot);

    Poller& poller_;
    std::string host_;
    int port_;
    std::string filter_;
    std::map<std::string, std::string> edits_; // the "new value" field of each row
    std::set<std::string> plotted_;

    // Trend data: per plotted variable, (seconds since start, value) points.
    struct Series {
        bool isBool = false; // BOOLs get their own 0/1 axis
        std::deque<std::pair<double, double>> points;
    };
    std::map<std::string, Series> history_;
    std::uint64_t lastSequence_ = 0;
    std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
};

} // namespace softplc::monitor
