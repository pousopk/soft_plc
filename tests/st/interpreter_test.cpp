#include <doctest/doctest.h>
#include "st/analyzer.hpp"
#include "st/interpreter.hpp"
#include <cmath>
#include <string>

using namespace softplc;
using namespace softplc::st;

// RuntimeError doesn't derive from std::exception, so tell doctest how to
// show it (used by CHECK_THROWS_WITH and in failure reports).
REGISTER_EXCEPTION_TRANSLATOR(const RuntimeError& error) { return doctest::String(error.message.c_str()); }

namespace {

// Compiles a program and runs it for a number of cycles, then lets the test
// look at variables and the process image.
class Runner {
public:
    explicit Runner(std::string_view source, std::uint64_t loopLimit = 1'000'000)
        : image_(8, 8, 32), interpreter_(compile(source), loopLimit) {}

    Runner& cycles(int count) {
        for (int i = 0; i < count; ++i) {
            interpreter_.runCycle(image_);
        }
        return *this;
    }

    std::int64_t integer(std::string_view name) const { return std::get<std::int64_t>(value(name)); }
    double real(std::string_view name) const { return std::get<double>(value(name)); }
    bool boolean(std::string_view name) const { return std::get<bool>(value(name)); }

    ProcessImage& image() { return image_; }

private:
    static ProgramUnit compile(std::string_view source) {
        auto program = compileProgram(source);
        if (!program) {
            FAIL("compile error: " << program.error().message);
        }
        return std::move(*program);
    }

    Value value(std::string_view name) const {
        auto v = interpreter_.valueOf(name, image_);
        REQUIRE(v.has_value());
        return *v;
    }

    ProcessImage image_;
    Interpreter interpreter_;
};

// A program with one INT `r` (and helpers), running `body` once.
Runner runOnce(std::string_view body) {
    Runner runner(std::string("PROGRAM T VAR r, a, b, i : INT; d : DINT; x : REAL; f : BOOL; END_VAR\n") +
                  std::string(body) + "\nEND_PROGRAM");
    runner.cycles(1);
    return runner;
}

} // namespace

TEST_CASE("Arithmetic follows precedence") {
    CHECK(runOnce("r := 2 + 3 * 4;").integer("r") == 14);
    CHECK(runOnce("r := (2 + 3) * 4;").integer("r") == 20);
    CHECK(runOnce("r := 10 - 4 - 3;").integer("r") == 3);
    CHECK(runOnce("r := -2 * 3;").integer("r") == -6);
}

TEST_CASE("Integer division truncates towards zero, like IEC") {
    CHECK(runOnce("r := 7 / 2;").integer("r") == 3);
    CHECK(runOnce("r := -7 / 2;").integer("r") == -3);
    CHECK(runOnce("r := 7 MOD 3;").integer("r") == 1);
    CHECK(runOnce("r := -7 MOD 2;").integer("r") == -1);
}

TEST_CASE("INT and DINT wrap around on overflow, like a PLC") {
    CHECK(runOnce("r := 32767; r := r + 1;").integer("r") == -32768);
    CHECK(runOnce("r := -32768; r := r - 1;").integer("r") == 32767);
    CHECK(runOnce("r := 300; r := r * r;").integer("r") == 90000 - 65536);
    CHECK(runOnce("d := 2147483647; d := d + 1;").integer("d") == -2147483648LL);
    CHECK(runOnce("d := 32767; d := d + 1;").integer("d") == 32768); // DINT has room
}

TEST_CASE("REAL arithmetic, mixed with integers") {
    CHECK(runOnce("x := 7 / 2.0;").real("x") == doctest::Approx(3.5));
    CHECK(runOnce("a := 3; x := a * 1.5;").real("x") == doctest::Approx(4.5));
    CHECK(runOnce("x := 2 ** 10;").real("x") == doctest::Approx(1024.0));
    CHECK(runOnce("x := 7;").real("x") == doctest::Approx(7.0)); // INT literal widened to REAL
}

TEST_CASE("REAL has 32-bit float precision") {
    // 0.1 can't be stored exactly; as a float it's a little more than 0.1.
    CHECK(runOnce("x := 0.1;").real("x") == static_cast<double>(0.1F));
    CHECK(runOnce("x := 16777216.0; x := x + 1.0;").real("x") == 16777216.0); // float runs out of digits
}

TEST_CASE("Comparisons and logic") {
    CHECK(runOnce("f := 3 > 2 AND NOT (1 = 2);").boolean("f"));
    CHECK_FALSE(runOnce("f := TRUE XOR TRUE;").boolean("f"));
    CHECK(runOnce("a := 3; x := 2.5; f := a > x;").boolean("f"));
    CHECK(runOnce("f := TRUE = (1 < 2);").boolean("f"));
}

TEST_CASE("IF picks the first true branch") {
    CHECK(runOnce("a := 5; IF a > 10 THEN r := 1; ELSIF a > 3 THEN r := 2; ELSIF a > 0 THEN r := 3; ELSE r := 4; "
                  "END_IF;")
              .integer("r") == 2);
    CHECK(runOnce("a := -1; IF a > 0 THEN r := 1; ELSE r := 4; END_IF;").integer("r") == 4);
    CHECK(runOnce("r := 9; IF FALSE THEN r := 1; END_IF;").integer("r") == 9);
}

TEST_CASE("CASE matches single labels, lists, ranges, and falls back to ELSE") {
    const char* code = "CASE a OF 1: r := 10; 2, 3: r := 20; 4..6: r := 30; -5..-1: r := 40; ELSE r := 99; END_CASE;";
    CHECK(runOnce(std::string("a := 1;") + code).integer("r") == 10);
    CHECK(runOnce(std::string("a := 3;") + code).integer("r") == 20);
    CHECK(runOnce(std::string("a := 6;") + code).integer("r") == 30);
    CHECK(runOnce(std::string("a := -3;") + code).integer("r") == 40);
    CHECK(runOnce(std::string("a := 7;") + code).integer("r") == 99);
}

TEST_CASE("FOR loops count up, count down, and may not run at all") {
    CHECK(runOnce("r := 0; FOR i := 1 TO 10 DO r := r + i; END_FOR;").integer("r") == 55);
    CHECK(runOnce("r := 0; FOR i := 10 TO 1 BY -3 DO r := r * 10 + i; END_FOR;").integer("r") == 10741);
    CHECK(runOnce("r := 0; FOR i := 5 TO 1 DO r := r + 1; END_FOR;").integer("r") == 0);
    CHECK(runOnce("r := 0; FOR i := 0 TO 20 BY 5 DO r := r + 1; END_FOR;").integer("r") == 5); // 0 5 10 15 20
}

TEST_CASE("FOR bounds are evaluated once, before the loop") {
    CHECK(runOnce("a := 3; r := 0; FOR i := 1 TO a DO a := 10; r := r + 1; END_FOR;").integer("r") == 3);
}

TEST_CASE("WHILE checks first; REPEAT runs at least once") {
    CHECK(runOnce("r := 0; WHILE r < 5 DO r := r + 1; END_WHILE;").integer("r") == 5);
    CHECK(runOnce("r := 0; WHILE FALSE DO r := 1; END_WHILE;").integer("r") == 0);
    CHECK(runOnce("r := 0; REPEAT r := r + 1; UNTIL TRUE END_REPEAT;").integer("r") == 1);
}

TEST_CASE("EXIT leaves only the innermost loop") {
    CHECK(runOnce("r := 0; FOR i := 1 TO 100 DO IF i = 4 THEN EXIT; END_IF; r := i; END_FOR;").integer("r") == 3);
    CHECK(runOnce("r := 0; FOR a := 1 TO 3 DO FOR b := 1 TO 3 DO IF b = 2 THEN EXIT; END_IF; r := r + 1; END_FOR; "
                  "END_FOR;")
              .integer("r") == 3);
}

TEST_CASE("RETURN ends the cycle early") {
    CHECK(runOnce("r := 1; RETURN; r := 2;").integer("r") == 1);
    CHECK(runOnce("r := 0; WHILE TRUE DO r := r + 1; IF r = 3 THEN RETURN; END_IF; END_WHILE; r := 99;")
              .integer("r") == 3);
}

TEST_CASE("Variables keep their values between cycles") {
    Runner runner("PROGRAM Counter VAR count : INT := 100; END_VAR count := count + 1; END_PROGRAM");
    CHECK(runner.integer("count") == 100); // the initial value, before the first cycle
    runner.cycles(5);
    CHECK(runner.integer("count") == 105);
}

TEST_CASE("Variables start at zero / FALSE without an initial value") {
    Runner runner("PROGRAM P VAR a : INT; b : BOOL; c : REAL; d : DINT := -7; END_VAR END_PROGRAM");
    CHECK(runner.integer("a") == 0);
    CHECK_FALSE(runner.boolean("b"));
    CHECK(runner.real("c") == 0.0);
    CHECK(runner.integer("d") == -7);
}

TEST_CASE("AT variables read and write the process image directly") {
    Runner runner("PROGRAM Io\n"
                  "VAR\n"
                  "    start AT %IX0.0 : BOOL;\n"
                  "    lamp AT %QX0.1 : BOOL;\n"
                  "    setpoint AT %IW2 : INT;\n"
                  "    doubled AT %QW2 : INT;\n"
                  "    total AT %MD0 : DINT;\n"
                  "    level AT %MD4 : REAL;\n"
                  "END_VAR\n"
                  "    lamp := start;\n"
                  "    doubled := setpoint * 2;\n"
                  "    total := -100000;\n"
                  "    level := 12.5;\n"
                  "END_PROGRAM");

    runner.image().writeBit(Area::Input, 0, 0, true);
    runner.image().writeWord(Area::Input, 2, static_cast<std::uint16_t>(-300));
    runner.cycles(1);

    CHECK(runner.image().readBit(Area::Output, 0, 1));
    CHECK(static_cast<std::int16_t>(runner.image().readWord(Area::Output, 2)) == -600);
    CHECK(static_cast<std::int32_t>(runner.image().readDWord(Area::Marker, 0)) == -100000);
    CHECK(runner.image().readDWord(Area::Marker, 4) == 0x41480000); // 12.5 as an IEEE 754 float
    CHECK(runner.real("level") == 12.5);                            // and read back through the variable

    runner.image().writeBit(Area::Input, 0, 0, false);
    runner.cycles(1);
    CHECK_FALSE(runner.image().readBit(Area::Output, 0, 1));
}

TEST_CASE("Division by zero is a runtime error with a location") {
    Runner runner("PROGRAM P VAR a, b : INT; END_VAR\n  a := 10 / b;\nEND_PROGRAM");
    try {
        runner.cycles(1);
        FAIL("expected a RuntimeError");
    } catch (const RuntimeError& error) {
        CHECK(error.message == "division by zero");
        CHECK(error.location == SourceLocation{2, 11});
    }
    Runner modulo("PROGRAM P VAR a, b : INT; END_VAR a := 10 MOD b; END_PROGRAM");
    CHECK_THROWS_AS(modulo.cycles(1), RuntimeError);
}

TEST_CASE("Dividing a REAL by zero gives infinity, not an error") {
    CHECK(std::isinf(runOnce("x := 1.0 / 0.0;").real("x")));
}

TEST_CASE("The watchdog stops endless loops") {
    Runner runner("PROGRAM P VAR a : INT; END_VAR WHILE TRUE DO a := a + 1; END_WHILE; END_PROGRAM", 1000);
    CHECK_THROWS_WITH_AS(runner.cycles(1), doctest::Contains("watchdog"), RuntimeError);

    // An INT loop variable can never pass 32767, so this loop never ends either.
    Runner wrapping("PROGRAM P VAR i, a : INT; END_VAR FOR i := 0 TO 32767 DO a := i; END_FOR; END_PROGRAM", 100'000);
    CHECK_THROWS_AS(wrapping.cycles(1), RuntimeError);
}

TEST_CASE("The watchdog counts per cycle, not in total") {
    Runner runner("PROGRAM P VAR i, a : INT; END_VAR FOR i := 1 TO 10 DO a := i; END_FOR; END_PROGRAM", 10);
    CHECK_NOTHROW(runner.cycles(5)); // 10 iterations per cycle is within the limit every time
}

TEST_CASE("FOR with BY 0 is a runtime error") {
    Runner runner("PROGRAM P VAR i, s : INT; END_VAR FOR i := 1 TO 5 BY s DO END_FOR; END_PROGRAM");
    CHECK_THROWS_WITH_AS(runner.cycles(1), doctest::Contains("BY 0"), RuntimeError);
}
