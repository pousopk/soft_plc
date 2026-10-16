#include <doctest/doctest.h>
#include "core/scan_cycle.hpp"
#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace softplc;
using namespace std::chrono_literals;

namespace {

// Records every call so tests can check the order of the cycle phases.
// Simulates one input (%I0.0) and captures the last written output (%Q0.0).
class FakeIo : public IoDriver {
public:
    explicit FakeIo(std::vector<std::string>& calls) : calls_(calls) {}

    void readInputs(ProcessImage& image) override {
        calls_.push_back("read");
        image.writeBit(Area::Input, 0, 0, inputValue);
    }
    void writeOutputs(const ProcessImage& image) override {
        calls_.push_back("write");
        lastOutput = image.readBit(Area::Output, 0, 0);
    }

    bool inputValue = false;
    bool lastOutput = false;

private:
    std::vector<std::string>& calls_;
};

// Copies %I0.0 to %Q0.0 and remembers the context of every cycle.
class CopyProgram : public Program {
public:
    explicit CopyProgram(std::vector<std::string>& calls) : calls_(calls) {}

    void execute(ProcessImage& image, const CycleContext& context) override {
        calls_.push_back("execute");
        contexts.push_back(context);
        image.writeBit(Area::Output, 0, 0, image.readBit(Area::Input, 0, 0));
    }

    std::vector<CycleContext> contexts;

private:
    std::vector<std::string>& calls_;
};

// Runs a fixed number of cycles, then asks the scan cycle to stop.
// Optionally burns time to force overruns.
class CountingProgram : public Program {
public:
    CountingProgram(std::atomic<bool>& stop, std::uint64_t stopAfter, Duration workTime = {})
        : stop_(stop), stopAfter_(stopAfter), workTime_(workTime) {}

    void execute(ProcessImage&, const CycleContext& context) override {
        if (workTime_ > Duration{}) {
            std::this_thread::sleep_for(workTime_);
        }
        if (context.cycleNumber + 1 >= stopAfter_) {
            stop_ = true;
        }
    }

private:
    std::atomic<bool>& stop_;
    std::uint64_t stopAfter_;
    Duration workTime_;
};

class NullIo : public IoDriver {
public:
    void readInputs(ProcessImage&) override {}
    void writeOutputs(const ProcessImage&) override {}
};

} // namespace

TEST_CASE("runOnce reads inputs, executes the program, then writes outputs") {
    std::vector<std::string> calls;
    ProcessImage image(1, 1, 0);
    FakeIo io(calls);
    CopyProgram program(calls);
    ScanCycle plc(image, io, program, 10ms);

    io.inputValue = true;
    plc.runOnce(Clock::now());

    CHECK(calls == std::vector<std::string>{"read", "execute", "write"});
    CHECK(io.lastOutput); // the input travelled through the image to the output in one cycle
}

TEST_CASE("runOnce passes the cycle timestamp and a cycle counter to the program") {
    std::vector<std::string> calls;
    ProcessImage image(1, 1, 0);
    FakeIo io(calls);
    CopyProgram program(calls);
    ScanCycle plc(image, io, program, 10ms);

    const auto t0 = TimePoint{} + 1s;
    plc.runOnce(t0);
    plc.runOnce(t0 + 10ms);
    plc.runOnce(t0 + 20ms);

    REQUIRE(program.contexts.size() == 3);
    CHECK(program.contexts[0].cycleNumber == 0);
    CHECK(program.contexts[2].cycleNumber == 2);
    CHECK(program.contexts[1].now == t0 + 10ms);
}

TEST_CASE("runOnce collects execution time statistics") {
    std::vector<std::string> calls;
    ProcessImage image(1, 1, 0);
    FakeIo io(calls);
    CopyProgram program(calls);
    ScanCycle plc(image, io, program, 10ms);

    for (int i = 0; i < 5; ++i) {
        plc.runOnce(Clock::now());
    }

    const auto& stats = plc.stats();
    CHECK(stats.cycles == 5);
    CHECK(stats.minExecution <= stats.averageExecution());
    CHECK(stats.averageExecution() <= stats.maxExecution);
    CHECK(stats.overruns == 0);
}

TEST_CASE("run keeps the cycle time and stops when asked") {
    std::atomic<bool> stop{false};
    ProcessImage image(0, 0, 0);
    NullIo io;
    CountingProgram program(stop, /*stopAfter=*/5);
    ScanCycle plc(image, io, program, 5ms);

    const auto started = Clock::now();
    plc.run(stop);
    const auto elapsed = Clock::now() - started;

    CHECK(plc.stats().cycles == 5);
    CHECK(plc.stats().overruns == 0);
    CHECK(elapsed >= 20ms); // 5 cycles = 4 full waits between them, plus the last one
}

TEST_CASE("run counts an overrun when the program takes longer than the cycle time") {
    std::atomic<bool> stop{false};
    ProcessImage image(0, 0, 0);
    NullIo io;
    CountingProgram program(stop, /*stopAfter=*/3, /*workTime=*/5ms);
    ScanCycle plc(image, io, program, 1ms);

    plc.run(stop);

    CHECK(plc.stats().cycles == 3);
    CHECK(plc.stats().overruns == 3);
}

TEST_CASE("run calls the idle task once after every cycle") {
    std::atomic<bool> stop{false};
    ProcessImage image(0, 0, 0);
    NullIo io;
    CountingProgram program(stop, /*stopAfter=*/4);
    ScanCycle plc(image, io, program, 1ms);

    std::vector<std::uint64_t> cyclesSeen;
    plc.setIdleTask([&] { cyclesSeen.push_back(plc.stats().cycles); });
    plc.run(stop);

    CHECK(cyclesSeen == std::vector<std::uint64_t>{1, 2, 3, 4});
}
