#include "core/process_image.hpp"
#include "core/scan_cycle.hpp"
#include "demo/blink_program.hpp"
#include "demo/console_io.hpp"
#include <atomic>
#include <chrono>
#include <csignal>
#include <fmt/format.h>

namespace {

std::atomic<bool> stopRequested{false};

extern "C" void onSignal(int) { stopRequested = true; }

double toMicroseconds(softplc::Duration d) { return std::chrono::duration<double, std::micro>(d).count(); }

} // namespace

int main() {
    using namespace std::chrono_literals;

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    softplc::ProcessImage image(/*inputBytes=*/16, /*outputBytes=*/16, /*markerBytes=*/256);
    softplc::ConsoleIo io(softplc::Clock::now());
    softplc::BlinkProgram program;
    softplc::ScanCycle plc(image, io, program, 10ms);

    fmt::print("soft_plc running, cycle time 10 ms. Press Ctrl+C to stop.\n");
    plc.run(stopRequested);

    const auto& stats = plc.stats();
    fmt::print("\nStopped after {} cycles (%MD0 = {}).\n", stats.cycles,
               image.readDWord(softplc::Area::Marker, 0));
    fmt::print("Execution time: avg {:.1f} us, min {:.1f} us, max {:.1f} us. Overruns: {}\n",
               toMicroseconds(stats.averageExecution()), toMicroseconds(stats.minExecution),
               toMicroseconds(stats.maxExecution), stats.overruns);
    return 0;
}
