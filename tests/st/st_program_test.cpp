#include <doctest/doctest.h>
#include "core/scan_cycle.hpp"
#include "st/analyzer.hpp"
#include "st/st_program.hpp"
#include "support/engine.hpp"
#include <chrono>

using namespace softplc;
using namespace softplc::st;
using namespace std::chrono_literals;

namespace {

class NullIo : public IoDriver {
public:
    void readInputs(ProcessImage&) override {}
    void writeOutputs(const ProcessImage&) override {}
};

CompilationUnit compile(std::string_view source) {
    auto program = compileProgram(source);
    REQUIRE(program.has_value());
    return std::move(*program);
}

} // namespace

TEST_CASE("An ST program runs inside the scan cycle") {
    ProcessImage image(1, 1, 0);
    NullIo io;
    StProgram program(compile("PROGRAM P VAR n : INT; lamp AT %QX0.0 : BOOL; END_VAR\n"
                              "n := n + 1; lamp := n >= 3; END_PROGRAM"),
                      {}, 1'000'000, testEngine());
    ScanCycle plc(image, io, program, 10ms);

    plc.runOnce(Clock::now());
    plc.runOnce(Clock::now());
    CHECK_FALSE(image.readBit(Area::Output, 0, 0));
    plc.runOnce(Clock::now());
    CHECK(image.readBit(Area::Output, 0, 0));
    CHECK_FALSE(program.stopped());
}

TEST_CASE("A runtime error puts the program in STOP and switches the outputs off") {
    ProcessImage image(0, 2, 0);
    NullIo io;
    int stopCalls = 0;
    std::string stopMessage;
    StProgram program(compile("PROGRAM P VAR lamp AT %QX0.0 : BOOL; n : INT := 2; r : INT; END_VAR\n"
                              "lamp := TRUE; n := n - 1; r := 10 / n; END_PROGRAM"),
                      [&](const RuntimeError& error) {
                          ++stopCalls;
                          stopMessage = error.message;
                      },
                      1'000'000, testEngine());
    ScanCycle plc(image, io, program, 10ms);
    image.writeByte(Area::Output, 1, 0xFF); // some other output that's also on

    plc.runOnce(Clock::now()); // n = 1: fine
    CHECK(image.readBit(Area::Output, 0, 0));
    CHECK_FALSE(program.stopped());

    plc.runOnce(Clock::now()); // n = 0: division by zero
    CHECK(program.stopped());
    CHECK(stopCalls == 1);
    CHECK(stopMessage == "division by zero");
    CHECK(image.readByte(Area::Output, 0) == 0); // every output off
    CHECK(image.readByte(Area::Output, 1) == 0);

    plc.runOnce(Clock::now()); // stays in STOP: no more execution, no second callback
    CHECK(stopCalls == 1);
    CHECK_FALSE(image.readBit(Area::Output, 0, 0));
}
