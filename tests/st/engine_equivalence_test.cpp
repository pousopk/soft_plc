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
            // Compared as text, strings included, so a mismatch is easy to read in the test output.
            const auto a = tree.interpreter.formatValueOf(variable.path, tree.image);
            const auto b = bytecode.interpreter.formatValueOf(variable.path, bytecode.image);
            REQUIRE(a.has_value());
            REQUIRE(b.has_value());
            REQUIRE(*a == *b);
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
    for (const char* name : {"traffic_light.st", "loops.st", "function_blocks.st", "s7_demo.st", "stop_demo.st", "inspection.st", "analog.st"}) {
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

TEST_CASE("Both engines agree on arrays and structs, in memory and placed with AT") {
    const auto error = runBothEngines(R"(
        TYPE
            Point : STRUCT x : REAL; y : REAL := 1.0; END_STRUCT;
            Sample : STRUCT
                ok : BOOL;
                nok : BOOL;
                count : INT;
                where : Point;
                history : ARRAY[0..3] OF DINT;
            END_STRUCT;
        END_TYPE

        FUNCTION_BLOCK Average
        VAR_INPUT values : ARRAY[1..8] OF INT; END_VAR
        VAR_OUTPUT mean : REAL; best : Point; END_VAR
        VAR i : INT; sum : DINT; END_VAR
            sum := 0;
            FOR i := 1 TO 8 DO sum := sum + values[i]; END_FOR;
            mean := sum / 8.0;
            best.x := mean; best.y := values[(sum MOD 8 + 8) MOD 8 + 1];
        END_FUNCTION_BLOCK

        PROGRAM Arrays
        VAR
            a AT %IW0 : INT;
            b AT %IW2 : INT;
            bits AT %IB2 : ARRAY[0..7] OF BOOL;
            samples : ARRAY[1..4] OF Sample;
            placed AT %DB3.DBB0 : ARRAY[1..2] OF Sample;
            ring : ARRAY[1..8] OF INT := [8(-1)];
            grid : ARRAY[1..3, -1..1] OF DINT;
            pos, i, j, k : INT;
            avg : Average;
            p, q : Point;
        END_VAR
            pos := (a MOD 8 + 8) MOD 8;
            ring[pos + 1] := b;
            i := (b MOD 4 + 4) MOD 4 + 1;
            samples[i].ok := bits[pos];
            samples[i].nok := NOT samples[i].ok;
            samples[i].count := samples[i].count + 1;
            samples[i].where.x := a / 100.0;
            samples[i].history[samples[i].count MOD 4] := a * b;
            placed[(i MOD 2) + 1] := samples[i];
            IF bits[7] THEN placed[1].where.y := placed[1].where.y + 1.0; END_IF;
            FOR j := 1 TO 3 DO
                FOR k := -1 TO 1 DO grid[j, k] := grid[j, k] + ring[(j * 3 + k + 8) MOD 8 + 1]; END_FOR;
            END_FOR;
            avg(values := ring);
            q := avg.best;
            IF q.x > p.x THEN p := q; END_IF;
        END_PROGRAM)",
                   2000);
    CHECK_FALSE(error.has_value()); // all 2000 cycles ran
}

TEST_CASE("Both engines stop with the same error for an index out of bounds") {
    const auto error = runBothEngines("PROGRAM P VAR n AT %IW0 : INT; a : ARRAY[0..199] OF INT; x : INT; END_VAR\n"
                                      "  x := a[n];\n"
                                      "END_PROGRAM",
                                      200);
    REQUIRE(error.has_value());
    CHECK(error->find("is outside the bounds 0..199 at 2:10") != std::string::npos);
}

TEST_CASE("Both engines agree on functions and standard functions") {
    const auto error = runBothEngines(R"(
        TYPE Point : STRUCT x : REAL; y : REAL; END_STRUCT; END_TYPE

        FUNCTION Scale : REAL
        VAR_INPUT raw : INT; low : REAL := 0.0; high : REAL := 100.0; END_VAR
            Scale := low + (high - low) * raw / 27648.0;
        END_FUNCTION

        FUNCTION Distance : REAL
        VAR_INPUT a, b : Point; END_VAR
            Distance := SQRT(EXPT(a.x - b.x, 2) + EXPT(a.y - b.y, 2));
        END_FUNCTION

        FUNCTION Pick : DINT
        VAR_INPUT values : ARRAY[0..3] OF DINT; index : INT; END_VAR
        VAR i : INT; best : DINT; END_VAR
            IF index < 0 THEN RETURN; END_IF;
            best := values[0];
            FOR i := 1 TO 3 DO best := MAX(best, values[i]); END_FOR;
            Pick := best + values[index MOD 4] + Twice(index);
        END_FUNCTION

        FUNCTION Twice : DINT VAR_INPUT x : DINT; END_VAR Twice := x * 2; END_FUNCTION

        PROGRAM Functions
        VAR
            a AT %IW0 : INT;
            b AT %IW2 : INT;
            g AT %IX0.0 : BOOL;
            r1, r2, r3 : REAL;
            i1, i2 : INT;
            d1, d2 : DINT;
            t1 : TIME;
            f1 : BOOL;
            p, q : Point;
            ring : ARRAY[0..3] OF DINT;
            n : INT;
        END_VAR
            r1 := Scale(raw := a) + Scale(raw := b, high := 10.0) + Scale(a, -1.0, 1.0);
            r2 := ABS(r1) + SQRT(ABS(a)) + LN(ABS(b) + 1.0) + LOG(ABS(a) + 1) + EXP(a / 10000.0);
            r3 := SIN(a / 1000.0) * COS(b / 1000.0) + TAN(0.5) + ASIN(0.5) + ACOS(-0.5) + ATAN(b / 3.0);
            i1 := DINT_TO_INT(INT_TO_DINT(a) * 3) + ABS(b);
            i2 := REAL_TO_INT(r1 * 100.0) + BOOL_TO_INT(g) + LIMIT(-5, a, 5);
            d1 := TRUNC(r2 * 1000.0) + ROUND(r1 * 7.5) + CEIL(r3) + FLOOR(-r3) + REAL_TO_DINT(Scale(raw := a) * 1000.0);
            d2 := MAX(a, b, 0) - MIN(a, b, d1) + MUX(ABS(a MOD 3), 10, 20, 30) + SEL(g, 1, 2);
            t1 := LIMIT(T#0ms, DINT_TO_TIME(INT_TO_DINT(a)), T#10s) + MAX(T#1s, DINT_TO_TIME(d2));
            f1 := INT_TO_BOOL(a MOD 2) XOR DINT_TO_BOOL(d2) OR SEL(g, f1, NOT f1);
            p.x := a; p.y := b; q.x := INT_TO_REAL(b); q.y := DINT_TO_REAL(TIME_TO_DINT(t1)) / 1000.0;
            r3 := r3 + Distance(p, q);
            n := (n + 1) MOD 4;
            ring[n] := a * b;
            d2 := d2 + Pick(ring, n) + Pick(values := ring, index := -1);
        END_PROGRAM)",
                                      2000);
    CHECK_FALSE(error.has_value());
}

TEST_CASE("Both engines stop with the same error for a MUX selector out of range") {
    const auto error = runBothEngines("PROGRAM P VAR k AT %IW0 : INT; x : INT; END_VAR\n"
                                      "  x := MUX(k MOD 4, 1, 2, 3);\n"
                                      "END_PROGRAM",
                                      200);
    REQUIRE(error.has_value());
    CHECK(error->find("is outside 0..2 at 2:8") != std::string::npos);
}

TEST_CASE("Both engines agree on the new integer and bit-string types") {
    runBothEngines(R"(
        PROGRAM Bits
        VAR
            b0 AT %IB0 : BYTE; k AT %IB1 : USINT; b1 AT %IB1 : BYTE; w AT %IW2 : WORD;
            s AT %IB0 : SINT; u AT %IW0 : UINT; ud AT %ID0 : UDINT;
            l : LINT := 7; acc : LINT; d : DINT; q : UINT; m : WORD; n : BYTE; f, g : BOOL; count : USINT;
            outB AT %QB0 : BYTE; outW AT %QW2 : WORD; mw AT %MW0 : WORD; md AT %MD4 : UDINT; ms AT %MB8 : SINT;
        END_VAR
            count := count + 1;
            l := l * 3 + ud;
            acc := acc + s * u;
            IF k <> 0 THEN
                q := u / k;
                d := s MOD k;
            END_IF;
            m := w AND NOT b0 OR (b1 XOR 16#5A);
            n := NOT b0;
            f := w > b0;
            g := m = w OR f;
            outB := n XOR b1;
            outW := m;
            mw := w OR f;
            md := ud + count;
            ms := s - 100;
            d := d - s;
            q := q - 1;
        END_PROGRAM
    )", 500);
}

TEST_CASE("Both engines agree on bits, bytes and words of variables, also with run-time indices") {
    runBothEngines(R"(
        PROGRAM Parts
        VAR
            in0 AT %IB0 : USINT; in1 AT %IW2 : WORD;
            words : ARRAY[0..3] OF WORD; located AT %MB0 : ARRAY[0..3] OF WORD;
            d : DWORD; l : LINT; i : USINT; f : BOOL; b : BYTE;
        END_VAR
            i := in0 MOD 4;
            words[i].%X1 := NOT words[i].%X1;
            located[i].%B1 := in1.%B0;
            f := located[(i + 1) MOD 4].%X9 XOR in1.%X15;
            d.%W1 := words[i];
            d.%X0 := f;
            l.%B7 := in1.%B1;
            b := d.%B2 OR located[i].%B0;
        END_PROGRAM
    )", 300);
}

TEST_CASE("Both engines agree on strings") {
    runBothEngines(R"(
        FUNCTION Label : STRING[12]
        VAR_INPUT on : BOOL; END_VAR
            IF on THEN Label := 'running'; ELSE Label := 'stopped'; END_IF;
        END_FUNCTION
        FUNCTION_BLOCK Keeper
        VAR_INPUT text : STRING[6]; END_VAR VAR_OUTPUT last, previous : STRING[6]; END_VAR
            IF text <> last THEN previous := last; last := text; END_IF;
        END_FUNCTION_BLOCK
        PROGRAM Texts
        VAR
            a AT %IX0.0 : BOOL; b AT %IX0.1 : BOOL; k AT %IB1 : USINT;
            names : ARRAY[0..3] OF STRING[8] := ['zero', 'one', 'two', 'three'];
            pick, shown : STRING[8]; status : STRING[12]; keep : Keeper; c : CHAR; w : WSTRING[4] := "w";
            ordered, same : BOOL; located AT %MB0 : STRING[6]; wide AT %MB8 : WSTRING[3];
        END_VAR
            pick := names[k MOD 4];
            status := Label(a);
            ordered := pick < names[(k + 1) MOD 4];
            same := Label(a) = Label(b);
            keep(text := pick);
            shown := keep.previous;
            IF b THEN located := status; ELSE located := pick; END_IF;
            wide := w;
            IF ordered THEN c := 'y'; ELSE c := 'n'; END_IF;
            names[k MOD 4] := status;
        END_PROGRAM
    )", 300);
}
