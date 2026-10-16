#include <doctest/doctest.h>
#include "st/analyzer.hpp"
#include "st/interpreter.hpp"
#include "util/file.hpp"
#include <chrono>
#include <optional>
#include <random>
#include <string>

using namespace softplc;
using namespace softplc::st;
using namespace std::chrono_literals;

// Differential testing: run the same program on both engines, with the same
// inputs, and check after every cycle that they agree on every variable and
// every byte of memory. If the VM had any instruction slightly wrong, some
// cycle would show a difference.

namespace {

struct Side {
    ProcessImage image;
    Interpreter interpreter;
    std::optional<std::string> error; // a runtime error, once it happened

    Side(CompilationUnit unit, Engine engine, const std::map<std::uint16_t, std::size_t>& dataBlocks)
        : image(4, 4, 32), interpreter(std::move(unit), 100'000, engine) {
        for (const auto& [number, size] : dataBlocks) {
            image.addDataBlock(number, size);
        }
    }

    void cycle(TimePoint now) {
        if (error) {
            return;
        }
        try {
            interpreter.runCycle(image, now);
        } catch (const RuntimeError& e) {
            error = e.message + " at " + std::to_string(e.location.line) + ":" + std::to_string(e.location.column);
        }
    }
};

// Returns the runtime error both engines stopped with, if any.
std::optional<std::string> runBothEngines(const std::string& source, int cycles) {
    auto unit = compileProgram(source);
    REQUIRE_MESSAGE(unit.has_value(), (unit ? "" : unit.error().message));
    const auto dataBlocks = dataBlocksUsedBy(*unit);
    Side tree(compileProgram(source).value(), Engine::TreeWalker, dataBlocks);
    Side bytecode(std::move(*unit), Engine::Bytecode, dataBlocks);

    std::mt19937 random(12345); // fixed seed: the same "random" inputs every run
    auto now = TimePoint{} + 1h;
    for (int cycle = 0; cycle < cycles; ++cycle) {
        CAPTURE(cycle);
        // Random inputs, the same on both sides, to drive every branch.
        for (std::size_t i = 0; i < 4; ++i) {
            const auto byte = static_cast<std::uint8_t>(random());
            tree.image.writeByte(Area::Input, i, byte);
            bytecode.image.writeByte(Area::Input, i, byte);
        }
        tree.cycle(now);
        bytecode.cycle(now);
        now += 10ms;

        REQUIRE(tree.error == bytecode.error);
        for (const auto& variable : tree.interpreter.variables()) {
            CAPTURE(variable.path);
            const auto a = tree.interpreter.valueOf(variable.path, tree.image);
            const auto b = bytecode.interpreter.valueOf(variable.path, bytecode.image);
            REQUIRE(a.has_value());
            REQUIRE(b.has_value());
            // Compare as text so a mismatch is easy to read in the test output.
            REQUIRE(formatValue(*a, variable.type) == formatValue(*b, variable.type));
        }
        for (const auto area : {Area::Input, Area::Output, Area::Marker}) {
            const auto x = tree.image.bytes(area);
            const auto y = bytecode.image.bytes(area);
            REQUIRE(std::equal(x.begin(), x.end(), y.begin(), y.end()));
        }
        REQUIRE(tree.image.dataBlocks() == bytecode.image.dataBlocks());
        if (tree.error) {
            break; // both stopped with the same error: nothing more to compare
        }
    }
    return tree.error;
}

std::string example(const std::string& name) {
    auto source = readFile(std::string(SOFT_PLC_SOURCE_DIR) + "/examples/" + name);
    REQUIRE_MESSAGE(source.has_value(), "missing example " << name);
    return *source;
}

} // namespace

TEST_CASE("Both engines agree on every example program") {
    for (const char* name : {"traffic_light.st", "loops.st", "function_blocks.st", "s7_demo.st", "stop_demo.st"}) {
        CAPTURE(name);
        runBothEngines(example(name), 700);
    }
}

TEST_CASE("Both engines agree on a program that uses every kind of operation") {
    // The inputs (a, b and the bits) are random every cycle, so branches,
    // overflows, divisions and loops all go different ways from cycle to cycle.
    runBothEngines(R"(
        FUNCTION_BLOCK Accumulator
        VAR_INPUT add : DINT; enable : BOOL := TRUE; END_VAR
        VAR_OUTPUT total : DINT; calls : INT; END_VAR
            IF NOT enable THEN RETURN; END_IF;
            calls := calls + 1;
            total := total + add;
        END_FUNCTION_BLOCK

        PROGRAM Stress
        VAR
            a AT %IW0 : INT;
            b AT %IW2 : INT;
            bit0 AT %IX0.0 : BOOL;
            bit1 AT %IX1.3 : BOOL;
            outW AT %QW0 : INT;
            outB AT %QX2.0 : BOOL;
            outD AT %MD0 : DINT;
            outR AT %MD4 : REAL;
            outT AT %MD8 : TIME;
            d : DINT;
            r, r2 : REAL;
            t : TIME := T#1s;
            i, j, k, sel, q : INT;
            count : DINT;
            flags : BOOL;
            acc1, acc2 : Accumulator;
            ton1 : TON; tp1 : TP; tof1 : TOF;
            ctu1 : CTU; ctd1 : CTD;
            trig : R_TRIG; ftrig : F_TRIG;
            sr1 : SR; rs1 : RS;
        END_VAR
            (* integer arithmetic, with INT wrapping around *)
            outW := a * b + a - b;
            d := a * b;
            d := d * 3 + 7;
            IF b <> 0 THEN q := a / b; outD := a MOD b; ELSE q := -a; END_IF;
            sel := a MOD 7;

            (* REAL, mixed with integers *)
            r := a / 3.5 + b * 0.25;
            r2 := r ** 2 - -r;
            IF r2 > 1000.0 THEN r2 := r2 / 1000.0; END_IF;
            outR := r2;

            (* TIME *)
            t := t + T#10ms * 2 - T#5ms;
            IF t > T#1m THEN t := T#0ms; END_IF;
            outT := t / 2;

            (* logic *)
            flags := (bit0 AND NOT bit1) OR (a > b XOR b >= 0);
            outB := flags = bit0;

            (* CASE with lists, ranges and negative labels *)
            CASE sel OF
                0: i := 1;
                1, 2: i := 2;
                3..5: i := 3;
                -6..-1: i := -1;
            ELSE
                i := 99;
            END_CASE;

            (* loops, with EXIT *)
            count := 0;
            FOR j := 0 TO (b MOD 50) BY 3 DO
                count := count + j;
                IF count > 200 THEN EXIT; END_IF;
            END_FOR;
            FOR j := 20 TO 0 BY -4 DO count := count - 1; END_FOR;
            k := 0;
            WHILE k < (a MOD 10) DO
                k := k + 1;
                IF k = 7 THEN EXIT; END_IF;
            END_WHILE;
            REPEAT k := k - 1; UNTIL k <= 0 END_REPEAT;

            (* function blocks: user-defined and standard *)
            acc1(add := a, enable := bit0);
            acc2(add := d);
            ton1(IN := bit0, PT := T#30ms);
            tp1(IN := bit1, PT := T#50ms);
            tof1(IN := bit0, PT := T#20ms);
            trig(CLK := bit1);
            ftrig(CLK := bit0);
            ctu1(CU := trig.Q, R := a > 30000, PV := 5);
            ctd1(CD := bit0, LD := ctd1.Q, PV := 3);
            sr1(S1 := bit0, R := bit1);
            rs1(S := bit0, R1 := bit1);

            IF acc1.calls > 100 THEN RETURN; END_IF;
            outD := outD + acc2.total MOD 1000;
        END_PROGRAM)",
                   2000);
}

TEST_CASE("Both engines stop with the same runtime error at the same place") {
    const auto error = runBothEngines("PROGRAM P VAR n AT %IW0 : INT; x : INT; i : INT; END_VAR\n"
                                      "  FOR i := 0 TO 3 DO x := 100 / (n MOD 4 - i); END_FOR;\n"
                                      "END_PROGRAM",
                                      200);
    REQUIRE(error.has_value()); // with random inputs, n MOD 4 - i hits 0 sooner or later
    CHECK(*error == "division by zero at 2:31");
}
