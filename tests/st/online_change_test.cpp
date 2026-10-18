#include "st/analyzer.hpp"
#include "st/st_program.hpp"
#include "support/engine.hpp"
#include <chrono>
#include <doctest/doctest.h>
#include <string>

using namespace softplc;
using namespace softplc::st;
using namespace std::chrono_literals;

// Online change: a running program is replaced by a new version, which
// keeps the memory the two have in common.

namespace {

CompilationUnit compile(const std::string& source) {
    auto unit = compileProgram(source);
    if (!unit) {
        FAIL("compile error: " << unit.error().message);
    }
    return std::move(*unit);
}

// A program with a counter, a running timer, a user function block and an output.
const char* kBefore = "FUNCTION_BLOCK Pump\n"
                      "VAR_INPUT run : BOOL; END_VAR\n"
                      "VAR_OUTPUT starts : INT; END_VAR\n"
                      "VAR edge : R_TRIG; END_VAR\n"
                      "    edge(CLK := run);\n"
                      "    IF edge.Q THEN starts := starts + 1; END_IF;\n"
                      "END_FUNCTION_BLOCK\n"
                      "PROGRAM Main\n"
                      "VAR\n"
                      "    count : INT;\n"
                      "    level : INT;\n"
                      "    t : TON;\n"
                      "    c : CTU;\n"
                      "    p : Pump;\n"
                      "    pulse : BOOL;\n"
                      "    lamp AT %QX0.0 : BOOL;\n"
                      "    removed : INT := 5;\n"
                      "END_VAR\n"
                      "    count := count + 1;\n"
                      "    pulse := NOT pulse;\n"
                      "    t(IN := TRUE, PT := T#1s);\n"
                      "    c(CU := pulse, PV := 1000);\n"
                      "    p(run := pulse);\n"
                      "    lamp := TRUE;\n"
                      "END_PROGRAM\n";

// The new version: count counts by 10, level became a REAL, `removed` is
// gone, and `added` is new.
const char* kAfter = "FUNCTION_BLOCK Pump\n"
                     "VAR_INPUT run : BOOL; END_VAR\n"
                     "VAR_OUTPUT starts : INT; END_VAR\n"
                     "VAR edge : R_TRIG; END_VAR\n"
                     "    edge(CLK := run);\n"
                     "    IF edge.Q THEN starts := starts + 1; END_IF;\n"
                     "END_FUNCTION_BLOCK\n"
                     "PROGRAM Main\n"
                     "VAR\n"
                     "    count : INT;\n"
                     "    level : REAL;\n"
                     "    t : TON;\n"
                     "    c : CTU;\n"
                     "    p : Pump;\n"
                     "    pulse : BOOL;\n"
                     "    lamp AT %QX0.0 : BOOL;\n"
                     "    added : INT := 7;\n"
                     "END_VAR\n"
                     "    count := count + 10;\n"
                     "    pulse := NOT pulse;\n"
                     "    t(IN := TRUE, PT := T#1s);\n"
                     "    c(CU := pulse, PV := 1000);\n"
                     "    p(run := pulse);\n"
                     "END_PROGRAM\n";

struct Running {
    ProcessImage image{4, 4, 8};
    StProgram program{compile(kBefore), {}, 1'000'000, testEngine()};
    TimePoint now = TimePoint{} + 1h;

    void cycles(int n) {
        for (int i = 0; i < n; ++i) {
            program.execute(image, CycleContext{now, 0});
            now += 10ms;
        }
    }
    std::int64_t integer(const char* path) const {
        return std::get<std::int64_t>(*program.interpreter().valueOf(path, image));
    }
};

} // namespace

TEST_CASE("Online change keeps what the versions share, and starts the rest over") {
    Running plc;
    plc.program.interpreter().setValue("level", std::int64_t{42}, plc.image);
    plc.cycles(40); // 400 ms: the timer is running, the counters have counted
    CHECK(plc.integer("count") == 40);
    CHECK(plc.integer("c.CV") == 20);
    CHECK(plc.integer("p.starts") == 20);
    CHECK(plc.integer("t.ET") == 390);

    const auto result = plc.program.replace(compile(kAfter));
    CHECK(result.initialized == std::vector<std::string>{"level", "added"});

    plc.cycles(1);
    CHECK(plc.integer("count") == 50);    // kept, and counting with the new code
    CHECK(plc.integer("c.CV") == 21);     // the CTU's own count went across (not 1: it didn't start over)
    CHECK(plc.integer("p.starts") == 21); // the user block too, with its R_TRIG
    CHECK(plc.integer("t.ET") == 400);    // the timer kept timing from where it was
    CHECK(plc.integer("added") == 7);     // new: its initial value
    CHECK(std::get<double>(*plc.program.interpreter().valueOf("level", plc.image)) == 0.0); // changed type
    CHECK(plc.image.readBit(Area::Output, 0, 0)); // AT: the process image stays

    plc.cycles(60);
    CHECK(plc.integer("t.ET") == 1000); // done at 1 s after its start, as if nothing had happened
}

TEST_CASE("Online change drops forces on variables that are gone, and leaves STOP") {
    Running plc;
    plc.program.force("removed", std::int64_t{9});
    plc.program.force("count", std::int64_t{3});
    plc.cycles(1);
    const auto result = plc.program.replace(compile(kAfter));
    CHECK(plc.program.forces().size() == 1);
    CHECK(plc.program.forces().count("count") == 1);
    CHECK(result.kept > 0);

    // A program that stopped on an error is running again after a change.
    ProcessImage image(1, 1, 1);
    StProgram broken(compile("PROGRAM Main\nVAR a : INT; b : INT; END_VAR\n    a := 1 / b;\nEND_PROGRAM\n"), {},
                     1'000'000, testEngine());
    broken.execute(image, CycleContext{TimePoint{}, 0});
    REQUIRE(broken.stopped());
    // (b keeps its value 0: the fix has to deal with it, as on a real PLC)
    broken.replace(compile("PROGRAM Main\nVAR a : INT; b : INT; END_VAR\n    a := 1 / (b + 1);\nEND_PROGRAM\n"));
    CHECK_FALSE(broken.stopped());
    broken.execute(image, CycleContext{TimePoint{}, 0});
    CHECK_FALSE(broken.stopped());
}

TEST_CASE("The online change example in the programming guide (chapter 8) does what it says") {
    const char* first = "PROGRAM Main\nVAR\n    count : INT;\n    t : TON;\n    lamp AT %QX0.0 : BOOL;\nEND_VAR\n"
                        "    count := count + 1;\n    t(IN := TRUE, PT := T#10s);\n    lamp := t.Q;\nEND_PROGRAM\n";
    const char* second = "PROGRAM Main\nVAR\n    count : INT;\n    t : TON;\n    lamp AT %QX0.0 : BOOL;\n"
                         "    tens : INT := 1;\nEND_VAR\n"
                         "    count := count + 10;\n    tens := count / 10;\n    t(IN := TRUE, PT := T#20s);\n"
                         "    lamp := t.Q;\nEND_PROGRAM\n";
    ProcessImage image(4, 4, 8);
    StProgram program(compile(first), {}, 1'000'000, testEngine());
    auto now = TimePoint{} + 1h;
    const auto cycles = [&](int n) {
        for (int i = 0; i < n; ++i) {
            program.execute(image, CycleContext{now, 0});
            now += 10ms;
        }
    };
    const auto integer = [&](const char* path) {
        return std::get<std::int64_t>(*program.interpreter().valueOf(path, image));
    };
    cycles(400); // 4 s
    CHECK(integer("count") == 400);

    const auto result = program.replace(compile(second));
    CHECK(result.kept == 6); // count, lamp, t.IN, t.PT, t.Q, t.ET
    CHECK(result.initialized == std::vector<std::string>{"tens"});
    CHECK(integer("tens") == 1);
    cycles(1);
    CHECK(integer("count") == 410);
    CHECK(integer("tens") == 41);
    CHECK(integer("t.ET") == 4000);
    cycles(1599); // up to 19.99 s after the start
    CHECK_FALSE(image.readBit(Area::Output, 0, 0));
    cycles(1); // 20 s: the new preset
    CHECK(image.readBit(Area::Output, 0, 0));
}
