#pragma once
#include "core/io_driver.hpp"
#include "core/plc_time.hpp"
#include "core/process_image.hpp"
#include "core/program.hpp"
#include <atomic>
#include <cstdint>
#include <functional>

namespace softplc {

struct CycleStats {
    std::uint64_t cycles = 0;
    std::uint64_t overruns = 0; // cycles whose work took longer than the cycle time
    Duration lastExecution{};
    Duration minExecution = Duration::max();
    Duration maxExecution{};
    Duration totalExecution{};

    Duration averageExecution() const {
        return cycles == 0 ? Duration{} : totalExecution / static_cast<Duration::rep>(cycles);
    }
};

// The heart of the PLC: read inputs -> execute program -> write outputs,
// repeated at a fixed cycle time.
//
// `run` keeps a fixed rhythm by sleeping until the next deadline rather than
// for a fixed amount, so the time spent working doesn't make the cycle drift.
// If a cycle takes longer than the cycle time, it counts as an overrun and the
// next cycle starts immediately.
class ScanCycle {
public:
    ScanCycle(ProcessImage& image, IoDriver& io, Program& program, Duration cycleTime);

    // Executes exactly one cycle with `now` as the cycle timestamp. Doesn't
    // sleep, which makes it easy to drive from tests.
    void runOnce(TimePoint now);

    // Runs cycles until `stopRequested` becomes true.
    void run(const std::atomic<bool>& stopRequested);

    // Work to do after every cycle, in the time left before the next one, on
    // the scan cycle's own thread. Real PLCs answer communication requests
    // the same way, between cycles, so the program never sees half a change.
    void setIdleTask(std::function<void()> task) { idleTask_ = std::move(task); }

    const CycleStats& stats() const { return stats_; }
    Duration cycleTime() const { return cycleTime_; }

private:
    ProcessImage& image_;
    IoDriver& io_;
    Program& program_;
    Duration cycleTime_;
    CycleStats stats_;
    std::function<void()> idleTask_;
};

} // namespace softplc
