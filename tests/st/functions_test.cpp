#include "st/analyzer.hpp"
#include "st/interpreter.hpp"
#include "st/parser.hpp"
#include "support/engine.hpp"
#include <chrono>
#include <cmath>
#include <limits>
#include <doctest/doctest.h>
#include <string>

using namespace softplc;
using namespace softplc::st;

REGISTER_EXCEPTION_TRANSLATOR(const RuntimeError& error) {
    return doctest::String(error.message.c_str());
}

namespace {

std::string errorOf(const std::string& source) {
    auto result = compileProgram(source);
    return result ? std::string() : result.error().message;
}

// Compiles and runs a program, then gives access to its variables.
class Runner {
public:
    explicit Runner(const std::string& source)
        : image_(8, 8, 64), interpreter_(compile(source), 1'000'000, testEngine()) {}

    Runner& cycles(int count) {
        for (int i = 0; i < count; ++i) {
            interpreter_.runCycle(image_, now_);
            now_ += std::chrono::milliseconds(10);
        }
        return *this;
    }

    Value value(std::string_view path) const {
        auto v = interpreter_.valueOf(path, image_);
        REQUIRE_MESSAGE(v.has_value(), "no variable " << std::string(path));
        return *v;
    }
    std::int64_t integer(std::string_view path) const { return std::get<std::int64_t>(value(path)); }
    double real(std::string_view path) const { return std::get<double>(value(path)); }
    bool boolean(std::string_view path) const { return std::get<bool>(value(path)); }

private:
    static CompilationUnit compile(const std::string& source) {
        auto unit = compileProgram(source);
        if (!unit) {
            FAIL("compile error: " << unit.error().message);
        }
        return std::move(*unit);
    }

    ProcessImage image_;
    Interpreter interpreter_;
    TimePoint now_ = TimePoint{} + std::chrono::seconds(100);
};

// Runs one statement that assigns `r` (of `type`) once and returns the runner.
Runner evaluate(const std::string& type, const std::string& expression) {
    Runner runner("PROGRAM P VAR r : " + type + "; i : INT := -7; d : DINT := 70000; x : REAL := 2.5; END_VAR\n" +
                  "r := " + expression + ";\nEND_PROGRAM");
    runner.cycles(1);
    return runner;
}

std::int64_t integerOf(const std::string& type, const std::string& expression) {
    return evaluate(type, expression).integer("r");
}
double realOf(const std::string& expression) {
    return evaluate("REAL", expression).real("r");
}
bool boolOf(const std::string& expression) {
    return evaluate("BOOL", expression).boolean("r");
}

constexpr const char* kScale = R"(
FUNCTION Scale : REAL
VAR_INPUT
    raw : INT;
    low : REAL := 0.0;
    high : REAL := 100.0;
END_VAR
    Scale := low + (high - low) * raw / 27648.0;
END_FUNCTION
)";

} // namespace

// ---------------------------------------------------------------- Parsing

TEST_CASE("FUNCTIONs and calls in expressions are parsed") {
    auto unit = parseProgram(std::string(kScale) + "PROGRAM P VAR v : REAL; END_VAR\n"
                                                   "  v := Scale(raw := 13824, high := 10.0) + MAX(1, 2);\n"
                                                   "END_PROGRAM");
    REQUIRE(unit.has_value());
    REQUIRE(unit->functions.size() == 1);
    const auto tree = dumpTree(*unit);
    CHECK(tree.find("Function Scale : REAL\n  VarInput\n    raw : INT\n    low : REAL := 0.0\n") != std::string::npos);
    CHECK(tree.find("    Assignment v\n"
                    "      Binary +\n"
                    "        Call Scale\n"
                    "          Argument raw\n"
                    "            Integer 13824\n"
                    "          Argument high\n"
                    "            Real 10.0\n"
                    "        Call MAX\n"
                    "          Argument\n"
                    "            Integer 1\n") != std::string::npos);
    CHECK(formatExpression(*parseExpression("LIMIT(MN := 0, IN := a + 1, MX := 10)").value()) ==
          "LIMIT(MN := 0, IN := (a + 1), MX := 10)");
}

TEST_CASE("Function syntax errors") {
    struct Case {
        const char* source;
        const char* messagePart;
    };
    const Case cases[] = {
        {"FUNCTION F VAR x : INT; END_VAR END_FUNCTION PROGRAM P END_PROGRAM",
         "expected ':' and the type the function returns, as in FUNCTION F : REAL"},
        {"FUNCTION F : INT F := 1; PROGRAM P END_PROGRAM", "expected 'END_FUNCTION' at the end of the function"},
        {"PROGRAM P VAR x : INT; END_VAR x := MAX(IN1 := 1, 2); END_PROGRAM",
         "in a call to MAX, give either every argument with its name (IN := ...) or none"},
        {"PROGRAM P VAR x : INT; END_VAR x := MAX(1, 2; END_PROGRAM", "expected ')' to close the call to MAX"},
    };
    for (const auto& c : cases) {
        CAPTURE(c.source);
        auto result = parseProgram(c.source);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().message.find(c.messagePart) != std::string::npos);
    }
}

// ---------------------------------------------------------------- Checks

TEST_CASE("Function rules are checked") {
    struct Case {
        std::string source;
        const char* message;
    };
    const std::string scale = kScale;
    const std::string vars = "PROGRAM P VAR r : REAL; i : INT; d : DINT; f : BOOL; t : TIME; a : ARRAY[1..3] OF INT; "
                             "timer : TON; END_VAR\n";
    const Case cases[] = {
        // Writing functions
        {"FUNCTION F : INT VAR_OUTPUT o : INT; END_VAR END_FUNCTION PROGRAM P END_PROGRAM",
         "a FUNCTION has no VAR_OUTPUT: it gives back one value, by assigning to its name, like F := ...;"},
        {"FUNCTION F : INT VAR t : TON; END_VAR END_FUNCTION PROGRAM P END_PROGRAM",
         "a FUNCTION has no memory between calls, so it can't contain function block instances like 't'; use a "
         "FUNCTION_BLOCK"},
        {"TYPE P2 : STRUCT x : REAL; END_STRUCT; END_TYPE FUNCTION F : P2 END_FUNCTION PROGRAM P END_PROGRAM",
         "a FUNCTION returns an elementary type like INT or REAL; give back arrays and structs through a function block's "
         "outputs"},
        {"FUNCTION F : INT VAR x AT %MW0 : INT; END_VAR END_FUNCTION PROGRAM P END_PROGRAM",
         "AT can only be used in the PROGRAM, not inside function 'F'"},
        {"FUNCTION F : INT VAR f : INT; END_VAR END_FUNCTION PROGRAM P END_PROGRAM",
         "'f' is the name of the function, which holds its result; choose another name"},
        {"FUNCTION F : INT F := F(); END_FUNCTION PROGRAM P END_PROGRAM",
         "function 'F' calls itself: F -> F (recursion isn't allowed)"},
        {"FUNCTION A : INT A := B(); END_FUNCTION FUNCTION B : INT B := A(); END_FUNCTION PROGRAM P END_PROGRAM",
         "function 'A' calls itself: A -> B -> A (recursion isn't allowed)"},
        {"FUNCTION MAX : INT END_FUNCTION PROGRAM P END_PROGRAM", "'MAX' is a standard function; choose another name"},
        {"FUNCTION TON : INT END_FUNCTION PROGRAM P END_PROGRAM",
         "'TON' is a standard function block; choose another name"},
        {"FUNCTION F : INT END_FUNCTION FUNCTION f : REAL END_FUNCTION PROGRAM P END_PROGRAM",
         "function 'f' is already defined on line 1"},
        {"FUNCTION F : INT END_FUNCTION TYPE F : STRUCT x : INT; END_STRUCT; END_TYPE PROGRAM P END_PROGRAM",
         "'F' is already the name of a function"},
        // Calling functions
        {scale + vars + "r := Scale(1, 2.0); END_PROGRAM", "Scale needs 3 arguments (raw, low, high), not 2"},
        {scale + vars + "r := Scale(raw := 1, middle := 2.0); END_PROGRAM",
         "Scale has no input called 'middle' (its inputs: raw, low, high)"},
        {scale + vars + "r := Scale(raw := 1, RAW := 2); END_PROGRAM", "'raw' is given twice"},
        {scale + vars + "r := Scale(raw := r); END_PROGRAM",
         "can't assign REAL to 'Scale.raw', which is INT (values might not fit)"},
        {scale + vars + "i := Scale(raw := 1); END_PROGRAM",
         "can't assign REAL to 'i', which is INT (values might not fit)"},
        {scale + vars + "Scale(raw := 1); END_PROGRAM",
         "the result of function 'Scale' isn't used: assign it, like x := Scale(...);"},
        {vars + "r := Missing(1); END_PROGRAM",
         "'Missing' is not a function: there's no standard function or FUNCTION with that name"},
        {vars + "f := timer(IN := TRUE); END_PROGRAM",
         "function block calls are statements: write 'timer(...);' on its own, then use its outputs like timer.Q"},
        {scale + "PROGRAM P VAR r : REAL := Scale(raw := 1); END_VAR END_PROGRAM",
         "initial values must be constants, not calls like 'Scale(...)'"},
        // Standard functions
        {vars + "i := ABS(f); END_PROGRAM", "ABS needs a number or a TIME, not BOOL"},
        {vars + "r := SQRT(t); END_PROGRAM", "SQRT needs a number, not TIME"},
        {vars + "r := SQRT(1.0, 2.0); END_PROGRAM", "SQRT needs 1 argument, not 2"},
        {vars + "i := MIN(1); END_PROGRAM", "MIN needs at least 2 arguments"},
        {vars + "i := MAX(i, t); END_PROGRAM",
         "MAX needs inputs of one kind: all numbers, all TIMEs or all bit strings; got INT and TIME"},
        {vars + "f := MAX(f, f); END_PROGRAM",
         "MAX needs inputs of one kind: all numbers, all TIMEs or all bit strings; got BOOL and BOOL"},
        {vars + "i := LIMIT(0, i); END_PROGRAM", "LIMIT needs 3 arguments, not 2"},
        {vars + "i := SEL(i, 1, 2); END_PROGRAM", "SEL's selector G must be BOOL, not INT"},
        {vars + "i := SEL(f, 1, t); END_PROGRAM",
         "SEL needs inputs of one kind: all numbers, all TIMEs, all bit strings or all BOOLs; got integer literal and TIME"},
        {vars + "i := MUX(r, 1, 2); END_PROGRAM", "MUX's selector K must be an integer, not REAL"},
        {vars + "i := MUX(i, 1); END_PROGRAM", "MUX needs a selector K and at least 2 inputs"},
        {vars + "i := DINT_TO_INT(f); END_PROGRAM", "can't assign BOOL to 'DINT_TO_INT', which is DINT"},
        {vars + "i := INT_TO_DINT(i); END_PROGRAM", "can't assign DINT to 'i', which is INT (values might not fit)"},
        {vars + "i := LIMIT(MN := 0, IN := i, TOP := 9); END_PROGRAM",
         "LIMIT has no input called 'TOP' (its inputs: MN, IN, MX)"},
        {vars + "i := LIMIT(MN := 0, IN := i, IN := 9); END_PROGRAM", "'IN' is given twice"},
        {vars + "r := ABS(a); END_PROGRAM", "ABS needs a number or a TIME, not ARRAY[1..3] OF INT"},
    };
    for (const auto& c : cases) {
        CAPTURE(c.source);
        CHECK(errorOf(c.source) == c.message);
    }
}

// ---------------------------------------------------------------- Running user functions

TEST_CASE("A FUNCTION computes a value from its inputs, by name or in order") {
    Runner run(std::string(kScale) + "PROGRAM P VAR a, b, c : REAL; END_VAR\n"
                                     "  a := Scale(raw := 13824);\n" // defaults: 0..100
                                     "  b := Scale(raw := 27648, high := 10.0);\n"
                                     "  c := Scale(6912, -50.0, 50.0);\n"
                                     "END_PROGRAM");
    run.cycles(1);
    CHECK(run.real("a") == doctest::Approx(50.0));
    CHECK(run.real("b") == doctest::Approx(10.0));
    CHECK(run.real("c") == doctest::Approx(-25.0));
}

TEST_CASE("A FUNCTION remembers nothing between calls") {
    Runner run("FUNCTION Next : INT\n"
               "VAR count : INT; END_VAR\n"
               "  count := count + 1;\n"
               "  Next := count;\n"
               "END_FUNCTION\n"
               "PROGRAM P VAR a, b : INT; END_VAR\n"
               "  a := Next(); b := Next();\n"
               "END_PROGRAM");
    run.cycles(3);
    CHECK(run.integer("a") == 1);
    CHECK(run.integer("b") == 1);
}

TEST_CASE("Functions call functions, also inside their own arguments") {
    Runner run("FUNCTION Double : DINT VAR_INPUT x : DINT; END_VAR Double := x * 2; END_FUNCTION\n"
               "FUNCTION Quad : DINT VAR_INPUT x : DINT; END_VAR Quad := Double(Double(x)); END_FUNCTION\n"
               "PROGRAM P VAR a, b : DINT; END_VAR\n"
               "  a := Double(Double(Double(1)));\n"
               "  b := Quad(5) + Double(x := 1);\n"
               "END_PROGRAM");
    run.cycles(1);
    CHECK(run.integer("a") == 8);
    CHECK(run.integer("b") == 22);
}

TEST_CASE("RETURN ends a function early; a result never assigned is 0") {
    Runner run("FUNCTION Clamp100 : INT VAR_INPUT x : INT; END_VAR\n"
               "  IF x > 100 THEN Clamp100 := 100; RETURN; END_IF;\n"
               "  Clamp100 := x;\n"
               "END_FUNCTION\n"
               "FUNCTION Nothing : REAL END_FUNCTION\n"
               "PROGRAM P VAR a, b : INT; c : REAL := 5.0; END_VAR\n"
               "  a := Clamp100(500); b := Clamp100(42); c := Nothing();\n"
               "END_PROGRAM");
    run.cycles(1);
    CHECK(run.integer("a") == 100);
    CHECK(run.integer("b") == 42);
    CHECK(run.real("c") == doctest::Approx(0.0));
}

TEST_CASE("Functions take arrays and structs, copied in") {
    Runner run(
        "TYPE Point : STRUCT x : REAL; y : REAL; END_STRUCT; END_TYPE\n"
        "FUNCTION Sum : DINT VAR_INPUT values : ARRAY[1..4] OF INT; END_VAR VAR i : INT; END_VAR\n"
        "  FOR i := 1 TO 4 DO Sum := Sum + values[i]; END_FOR;\n"
        "  values[1] := 0;\n" // changes the function's copy only
        "END_FUNCTION\n"
        "FUNCTION Length : REAL VAR_INPUT p : Point; END_VAR Length := SQRT(p.x * p.x + p.y * p.y); END_FUNCTION\n"
        "PROGRAM P VAR data : ARRAY[1..4] OF INT := [1, 2, 3, 4]; total : DINT; p : Point := (x := 3.0, y := 4.0);\n"
        "  len : REAL; rows : ARRAY[1..2] OF Point; i : INT := 2; END_VAR\n"
        "  total := Sum(data);\n"
        "  len := Length(p);\n"
        "  rows[i] := p; rows[i].x := 6.0; rows[i].y := 8.0;\n"
        "  len := len + Length(rows[i]);\n"
        "END_PROGRAM");
    run.cycles(1);
    CHECK(run.integer("total") == 10);
    CHECK(run.integer("data[1]") == 1);
    CHECK(run.real("len") == doctest::Approx(15.0));
}

TEST_CASE("Functions can be used from function blocks and loops") {
    Runner run(
        "FUNCTION Square : DINT VAR_INPUT x : DINT; END_VAR Square := x * x; END_FUNCTION\n"
        "FUNCTION_BLOCK SumOfSquares VAR_INPUT n : INT; END_VAR VAR_OUTPUT total : DINT; END_VAR VAR i : INT; END_VAR\n"
        "  total := 0;\n"
        "  FOR i := 1 TO n DO total := total + Square(i); END_FOR;\n"
        "END_FUNCTION_BLOCK\n"
        "PROGRAM P VAR s : SumOfSquares; result : DINT; END_VAR\n"
        "  s(n := 10); result := s.total;\n"
        "END_PROGRAM");
    run.cycles(1);
    CHECK(run.integer("result") == 385);
}

// ---------------------------------------------------------------- Standard functions

TEST_CASE("ABS keeps the type, and wraps for the smallest INT like a PLC") {
    CHECK(integerOf("INT", "ABS(i)") == 7);
    CHECK(realOf("ABS(-2.5)") == doctest::Approx(2.5));
    CHECK(integerOf("TIME", "ABS(-T#2s)") == 2000);
    CHECK(integerOf("INT", "ABS(-32767 - 1)") == -32768);
}

TEST_CASE("Mathematical functions give REALs") {
    CHECK(realOf("SQRT(16)") == doctest::Approx(4.0));
    CHECK(realOf("SQRT(2.0)") == static_cast<double>(std::sqrt(2.0F))); // 32-bit float precision
    CHECK(realOf("LN(EXP(1.0))") == doctest::Approx(1.0));
    CHECK(realOf("LOG(1000.0)") == doctest::Approx(3.0));
    CHECK(realOf("SIN(0.0) + COS(0.0)") == doctest::Approx(1.0));
    CHECK(realOf("ATAN(1.0) * 4.0") == doctest::Approx(3.14159265).epsilon(1e-6));
    CHECK(realOf("EXPT(2, 10)") == doctest::Approx(1024.0));
    CHECK(std::isnan(realOf("SQRT(-1.0)")));
}

TEST_CASE("TRUNC, ROUND, CEIL and FLOOR give DINTs") {
    CHECK(integerOf("DINT", "TRUNC(-2.7)") == -2);
    CHECK(integerOf("DINT", "TRUNC(2.7)") == 2);
    CHECK(integerOf("DINT", "ROUND(2.4)") == 2);
    CHECK(integerOf("DINT", "ROUND(2.5)") == 2); // halves go to the even number
    CHECK(integerOf("DINT", "ROUND(3.5)") == 4);
    CHECK(integerOf("DINT", "ROUND(-2.5)") == -2);
    CHECK(integerOf("DINT", "ROUND(-2.6)") == -3);
    CHECK(integerOf("DINT", "CEIL(2.1)") == 3);
    CHECK(integerOf("DINT", "FLOOR(-2.1)") == -3);
    CHECK(integerOf("DINT", "TRUNC(1.0E12)") == 2147483647); // clamped to DINT
}

TEST_CASE("MIN, MAX and LIMIT work on any numbers and on TIMEs") {
    CHECK(integerOf("INT", "MIN(4, i, 9)") == -7);
    CHECK(integerOf("DINT", "MAX(i, d)") == 70000);
    CHECK(realOf("MAX(i, x, 1)") == doctest::Approx(2.5)); // mixed: compared as REAL
    CHECK(integerOf("TIME", "MAX(T#1s, T#2s, T#500ms)") == 2000);
    CHECK(integerOf("INT", "LIMIT(0, i, 10)") == 0);
    CHECK(integerOf("INT", "LIMIT(0, 50, 10)") == 10);
    CHECK(integerOf("INT", "LIMIT(MN := 0, IN := 5, MX := 10)") == 5);
    CHECK(integerOf("INT", "MIN(IN2 := 3, IN1 := 8)") == 3);
}

TEST_CASE("SEL and MUX choose an input") {
    CHECK(integerOf("INT", "SEL(TRUE, 1, 2)") == 2);
    CHECK(integerOf("INT", "SEL(G := FALSE, IN0 := 1, IN1 := 2)") == 1);
    CHECK(boolOf("SEL(i > 0, TRUE, FALSE)"));
    CHECK(integerOf("INT", "MUX(2, 10, 20, 30)") == 30);
    CHECK(integerOf("TIME", "MUX(0, T#1s, T#2s)") == 1000);
    CHECK_THROWS_WITH(integerOf("INT", "MUX(i, 10, 20, 30)"), "MUX selector K = -7 is outside 0..2");
}

TEST_CASE("Conversions between the elementary types") {
    CHECK(integerOf("INT", "DINT_TO_INT(d)") == 70000 - 65536); // keeps the low 16 bits, like a PLC
    CHECK(integerOf("INT", "DINT_TO_INT(1234)") == 1234);
    CHECK(integerOf("INT", "REAL_TO_INT(x)") == 2);         // 2.5: halves go to the even number
    CHECK(integerOf("INT", "REAL_TO_INT(1.0E9)") == 32767); // clamped
    CHECK(integerOf("DINT", "REAL_TO_DINT(-1.0E12)") == -2147483648LL);
    CHECK(integerOf("DINT", "REAL_TO_DINT(-3.7)") == -4);
    CHECK(integerOf("DINT", "INT_TO_DINT(i)") == -7);
    CHECK(realOf("INT_TO_REAL(i)") == doctest::Approx(-7.0));
    CHECK(realOf("DINT_TO_REAL(16777217)") == 16777216.0); // REAL has 24 bits of precision
    CHECK(boolOf("INT_TO_BOOL(i)"));
    CHECK_FALSE(boolOf("DINT_TO_BOOL(0)"));
    CHECK(integerOf("INT", "BOOL_TO_INT(TRUE)") == 1);
    CHECK(integerOf("DINT", "TIME_TO_DINT(T#1m)") == 60000);
    CHECK(integerOf("TIME", "DINT_TO_TIME(1500)") == 1500);
    CHECK(integerOf("INT", "real_to_int(-0.4)") == 0); // names are case-insensitive
}

TEST_CASE("A MUX selector out of range stops the PLC at the call") {
    Runner run("PROGRAM P VAR k : INT := 1; x : INT; END_VAR\n"
               "  k := k + 1;\n"
               "  x := MUX(k, 1, 2, 3);\n"
               "END_PROGRAM");
    run.cycles(1); // k = 2: fine
    try {
        run.cycles(1);
        FAIL("expected a runtime error");
    } catch (const RuntimeError& error) {
        CHECK(error.message == "MUX selector K = 3 is outside 0..2");
        CHECK(error.location.line == 3);
        CHECK(error.location.column == 8);
    }
}

TEST_CASE("Conversions between bit strings and integers keep the bits") {
    CHECK(integerOf("INT", "WORD_TO_INT(16#FFFF)") == -1);
    CHECK(integerOf("WORD", "INT_TO_WORD(-1)") == 65535);
    CHECK(integerOf("WORD", "BYTE_TO_WORD(BYTE#16#AB)") == 0xAB);
    CHECK(integerOf("BYTE", "WORD_TO_BYTE(16#1234)") == 0x34);
    CHECK(integerOf("UDINT", "DWORD_TO_UDINT(16#FFFFFFFF)") == 4294967295);
    CHECK(integerOf("USINT", "REAL_TO_USINT(300.0)") == 255);
    CHECK(integerOf("SINT", "REAL_TO_SINT(-300.0)") == -128);
    CHECK(integerOf("LINT", "REAL_TO_LINT(1.0E30)") == std::numeric_limits<std::int64_t>::max());
    CHECK(integerOf("LINT", "REAL_TO_LINT(-1.0E30)") == std::numeric_limits<std::int64_t>::min());
    CHECK(realOf("UDINT_TO_REAL(UDINT#4000000000)") == doctest::Approx(4.0e9));
    CHECK(boolOf("WORD_TO_BOOL(WORD#16#0100)"));
    CHECK(integerOf("BYTE", "BOOL_TO_BYTE(TRUE)") == 1);
    CHECK(integerOf("LINT", "UINT_TO_LINT(UINT#65535)") == 65535);
}

TEST_CASE("DWORD_TO_REAL and REAL_TO_DWORD copy the 32 bits unchanged") {
    CHECK(realOf("DWORD_TO_REAL(16#3F800000)") == 1.0);
    CHECK(realOf("DWORD_TO_REAL(16#C0490FDB)") == doctest::Approx(-3.14159265));
    CHECK(integerOf("DWORD", "REAL_TO_DWORD(1.0)") == 0x3F800000);
    CHECK(integerOf("DWORD", "REAL_TO_DWORD(-2.0)") == 0xC0000000);
}

TEST_CASE("SHL, SHR, ROL and ROR shift and rotate bit strings") {
    CHECK(integerOf("BYTE", "SHL(IN := BYTE#16#81, N := 1)") == 0x02);
    CHECK(integerOf("WORD", "SHR(WORD#16#8000, 15)") == 1);
    CHECK(integerOf("BYTE", "SHL(BYTE#1, 8)") == 0);
    CHECK(integerOf("DWORD", "SHR(DWORD#16#FFFFFFFF, 40)") == 0);
    CHECK(integerOf("BYTE", "ROL(BYTE#16#81, 1)") == 0x03);
    CHECK(integerOf("BYTE", "ROR(BYTE#16#01, 9)") == 0x80);
    CHECK(integerOf("WORD", "ROR(WORD#16#00F1, 4)") == 0x100F);
    CHECK(integerOf("DWORD", "ROL(DWORD#16#80000001, 1)") == 3);
    CHECK(integerOf("BYTE", "SHL(BYTE#16#81, -1)") == 0x81); // a negative N changes nothing
}

TEST_CASE("Conversion and shift errors") {
    const std::string vars = "PROGRAM P VAR i : INT; w : WORD; r : REAL; END_VAR\n";
    CHECK(errorOf(vars + "w := SHL(i, 1); END_PROGRAM") == "SHL needs a bit string (BYTE, WORD or DWORD) for IN, not INT");
    CHECK(errorOf(vars + "w := SHL(w, r); END_PROGRAM") == "SHL needs an integer for N, not REAL");
    CHECK(errorOf(vars + "w := SHL(w); END_PROGRAM") == "SHL needs 2 arguments, not 1");
    CHECK(errorOf(vars + "w := REAL_TO_WORD(r); END_PROGRAM") ==
          "'REAL_TO_WORD' is not a function: there's no standard function or FUNCTION with that name");
    CHECK(errorOf(vars + "i := TIME_TO_INT(T#1s); END_PROGRAM") ==
          "'TIME_TO_INT' is not a function: there's no standard function or FUNCTION with that name");
}
