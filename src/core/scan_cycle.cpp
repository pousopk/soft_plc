#include "core/scan_cycle.hpp"
#include <algorithm>
#include <thread>

namespace softplc {

ScanCycle::ScanCycle(ProcessImage& image, IoDriver& io, Program& program, Duration cycleTime)
    : image_(image), io_(io), program_(program), cycleTime_(cycleTime) {}

void ScanCycle::runOnce(TimePoint now) {
    const auto started = Clock::now();

    io_.readInputs(image_);
    program_.execute(image_, CycleContext{now, stats_.cycles});
    io_.writeOutputs(image_);

    const auto elapsed = Clock::now() - started;
    ++stats_.cycles;
    stats_.lastExecution = elapsed;
    stats_.minExecution = std::min(stats_.minExecution, elapsed);
    stats_.maxExecution = std::max(stats_.maxExecution, elapsed);
    stats_.totalExecution += elapsed;
}

void ScanCycle::run(const std::atomic<bool>& stopRequested) {
    auto deadline = Clock::now();
    while (!stopRequested.load()) {
        runOnce(Clock::now());

        deadline += cycleTime_;
        const auto finished = Clock::now();
        if (finished > deadline) {
            // Too slow: start the next cycle right away and re-align the rhythm
            // from here, instead of trying to "catch up" with a burst of cycles.
            ++stats_.overruns;
            deadline = finished;
        }
        std::this_thread::sleep_until(deadline);
    }
}

} // namespace softplc
