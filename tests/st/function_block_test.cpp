#include <doctest/doctest.h>
#include "st/analyzer.hpp"
#include "st/interpreter.hpp"
#include "st/lexer.hpp"
#include "st/parser.hpp"
#include <chrono>
#include <string>

using namespace softplc;
using namespace softplc::st;
using namespace std::chrono_literals;

// Function blocks and TIME: from tokens, through the parser and the analyzer,
// to running instances.

namespace {

std::string errorOf(std::string_view source) {
    auto result = compileProgram(source);
    return result ? std::string() : result.error().message;
}

// Compiles and runs a program, with a clock that advances 10 ms per cycle.
class Runner {
public:
    explicit Runner(std::string_view source) : image_(4, 4, 8), interpreter_(compile(source)) {}

    Runner& cycles(int count) {
        for (int i = 0; i < count; ++i) {
            interpreter_.runCycle(image_, now_);
            now_ += 10ms;
        }
        return *this;
    }

    Value value(std::string_view name) const {
        auto v = interpreter_.valueOf(name, image_);
        REQUIRE_MESSAGE(v.has_value(), "no variable " << std::string(name));
        return *v;
    }
    bool boolean(std::string_view name) const { return std::get<bool>(value(name)); }
    std::int64_t integer(std::string_view name) const { return std::get<std::int64_t>(value(name)); }

private:
    static CompilationUnit compile(std::string_view source) {
        auto unit = compileProgram(source);
        if (!unit) {
            FAIL("compile error: " << unit.error().message);
        }
        return std::move(*unit);
    }

    ProcessImage image_;
    Interpreter interpreter_;
    TimePoint now_ = TimePoint{} + 1h;
};

const char* kCounterBlock = "FUNCTION_BLOCK Counter\n"
                            "VAR_INPUT step : INT := 1; END_VAR\n"
                            "VAR_OUTPUT total : INT; END_VAR\n"
                            "VAR calls : INT; END_VAR\n"
                            "    calls := calls + 1;\n"
                            "    total := total + step;\n"
                            "END_FUNCTION_BLOCK\n";

} // namespace

// ---------------------------------------------------------------- Lexer

TEST_CASE("TIME literals are converted to milliseconds") {
    const auto ms = [](std::string_view text) {
        auto tokens = tokenize(text);
        REQUIRE(tokens.has_value());
        REQUIRE((*tokens)[0].kind == TokenKind::Time);
        return std::get<std::int64_t>((*tokens)[0].value);
    };
    CHECK(ms("T#500ms") == 500);
    CHECK(ms("T#2s") == 2000);
    CHECK(ms("t#1m30s") == 90'000);
    CHECK(ms("TIME#1h2m3s4ms") == 3'723'004);
    CHECK(ms("T#1d") == 86'400'000);
    CHECK(ms("T#1_000ms") == 1000);
}

TEST_CASE("Malformed TIME literals are rejected") {
    for (const char* text : {"T#", "T#5", "T#5x", "T#1.5s", "T#5sx"}) {
        CAPTURE(text);
        CHECK_FALSE(tokenize(text).has_value());
    }
    // A variable that happens to be called T is still fine
    auto tokens = tokenize("T := 1;");
    REQUIRE(tokens.has_value());
    CHECK((*tokens)[0].kind == TokenKind::Identifier);
}

// ---------------------------------------------------------------- Parser

TEST_CASE("A function block definition, an instance and a call") {
    auto unit = parseProgram(std::string(kCounterBlock) + "PROGRAM Main\n"
                                                          "VAR c : Counter; t : TON; x : INT; END_VAR\n"
                                                          "    c(step := 2);\n"
                                                          "    t(IN := TRUE, PT := T#1s);\n"
                                                          "    x := c.total;\n"
                                                          "END_PROGRAM\n");
    REQUIRE(unit.has_value());
    REQUIRE(unit->functionBlocks.size() == 1);
    CHECK(dumpTree(unit->functionBlocks[0]) == "FunctionBlock Counter\n"
                                               "  VarInput\n"
                                               "    step : INT := 1\n"
                                               "  VarOutput\n"
                                               "    total : INT\n"
                                               "  Var\n"
                                               "    calls : INT\n"
                                               "  Body\n"
                                               "    Assignment calls\n"
                                               "      Binary +\n"
                                               "        Variable calls\n"
                                               "        Integer 1\n"
                                               "    Assignment total\n"
                                               "      Binary +\n"
                                               "        Variable total\n"
                                               "        Variable step\n");
    CHECK(dumpTree(unit->program).find("    Call t\n"
                                       "      Argument IN\n"
                                       "        Bool TRUE\n"
                                       "      Argument PT\n"
                                       "        Time T#1s\n"
                                       "    Assignment x\n"
                                       "      Member c.total\n") != std::string::npos);
}

TEST_CASE("A call can have no arguments") {
    auto statements = parseStatements("t();");
    REQUIRE(statements.has_value());
    CHECK(dumpTree((*statements)[0]) == "Call t\n");
}

TEST_CASE("Function block syntax errors") {
    struct Case {
        const char* source;
        const char* messagePart;
    };
    const Case cases[] = {
        {"t.IN := TRUE;", "can't assign to a member of 't'"},
        {"x := t(IN := TRUE);", "function block calls are statements"},
        {"t(IN TRUE);", "expected ':=' after 'IN'"},
        {"t(IN := TRUE;", "expected ')' to close the call"},
        {"t(IN := TRUE)", "expected ';' after the call"},
        {"x := t.;", "expected a member name after 't.'"},
    };
    for (const auto& c : cases) {
        CAPTURE(c.source);
        auto result = parseStatements(c.source);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().message.find(c.messagePart) != std::string::npos);
    }
    auto missingEnd = parseProgram("FUNCTION_BLOCK F x := 1; PROGRAM P END_PROGRAM");
    REQUIRE_FALSE(missingEnd.has_value());
    CHECK(missingEnd.error().message == "expected 'END_FUNCTION_BLOCK' at the end of the function block, found "
                                        "'PROGRAM'");
}

// ---------------------------------------------------------------- Analyzer

TEST_CASE("Function block rules are checked") {
    const std::string fb = kCounterBlock;
    struct Case {
        std::string source;
        const char* message;
    };
    const Case cases[] = {
        {"PROGRAM P VAR c : Missing; END_VAR END_PROGRAM",
         "unknown type 'Missing' (not BOOL, INT, DINT, REAL, TIME or a known function block)"},
        {"PROGRAM P VAR x : INT; END_VAR x(); END_PROGRAM", "'x' is not a function block instance, so it can't be called"},
        {"PROGRAM P VAR x, y : INT; END_VAR y := x.Q; END_PROGRAM",
         "'x' is not a function block instance, so it has no members"},
        {"PROGRAM P VAR t : TON; END_VAR t(START := TRUE); END_PROGRAM", "TON has no input called 'START'"},
        {"PROGRAM P VAR t : TON; END_VAR t(Q := TRUE); END_PROGRAM",
         "'Q' is an output of TON: read it after the call as t.Q"},
        {"PROGRAM P VAR t : TON; END_VAR t(IN := TRUE, IN := FALSE); END_PROGRAM", "'IN' is given twice"},
        {"PROGRAM P VAR t : TON; END_VAR t(IN := 1); END_PROGRAM",
         "can't assign integer literal to 't.IN', which is BOOL"},
        {"PROGRAM P VAR t : TON; END_VAR t(PT := 500); END_PROGRAM",
         "can't assign integer literal to 't.PT', which is TIME (write times like T#500ms)"},
        {"PROGRAM P VAR t : TON; x : BOOL; END_VAR x := t.Missing; END_PROGRAM",
         "TON has no input or output called 'Missing'"},
        {fb + "PROGRAM P VAR c : Counter; x : INT; END_VAR x := c.calls; END_PROGRAM",
         "'calls' is internal to Counter; only its inputs and outputs can be read"},
        {fb + "PROGRAM P VAR c : Counter; END_VAR c(calls := 1); END_PROGRAM",
         "'calls' is internal to Counter; only its inputs can be set in a call"},
        {"PROGRAM P VAR t : TON; x : BOOL; END_VAR x := t; END_PROGRAM",
         "'t' is a TON instance, not a value; use one of its outputs, like t.Q"},
        {"PROGRAM P VAR t : TON; END_VAR t := 1; END_PROGRAM", "can't assign to 't': it's a TON instance"},
        {"PROGRAM P VAR_INPUT x : INT; END_VAR END_PROGRAM",
         "a PROGRAM can only have VAR blocks; VAR_INPUT and VAR_OUTPUT are for function blocks"},
        {"FUNCTION_BLOCK F VAR x AT %MW0 : INT; END_VAR END_FUNCTION_BLOCK PROGRAM P END_PROGRAM",
         "AT can only be used in the PROGRAM, not inside function block 'F'"},
        {"FUNCTION_BLOCK F VAR_INPUT t : TON; END_VAR END_FUNCTION_BLOCK PROGRAM P END_PROGRAM",
         "inputs and outputs must be BOOL, INT, DINT, REAL or TIME, not function blocks"},
        {"FUNCTION_BLOCK TON END_FUNCTION_BLOCK PROGRAM P END_PROGRAM",
         "'TON' is a standard function block; choose another name"},
        {"FUNCTION_BLOCK F END_FUNCTION_BLOCK FUNCTION_BLOCK F END_FUNCTION_BLOCK PROGRAM P END_PROGRAM",
         "function block 'F' is already defined on line 1"},
        {"FUNCTION_BLOCK A VAR b : B; END_VAR END_FUNCTION_BLOCK FUNCTION_BLOCK B VAR a : A; END_VAR "
         "END_FUNCTION_BLOCK PROGRAM P END_PROGRAM",
         "function block 'A' contains itself: A -> B -> A"},
        {"FUNCTION_BLOCK F VAR f : F; END_VAR END_FUNCTION_BLOCK PROGRAM P END_PROGRAM",
         "function block 'F' contains itself: F -> F"},
        {"FUNCTION_BLOCK F x := 1; END_FUNCTION_BLOCK PROGRAM P VAR x : INT; END_VAR END_PROGRAM",
         "'x' is not declared (add it to a VAR block)"}, // each POU only sees its own variables
    };
    for (const auto& c : cases) {
        CAPTURE(c.source);
        CHECK(errorOf(c.source) == c.message);
    }
}

TEST_CASE("TIME arithmetic and comparisons are type-checked") {
    const auto program = [](const char* body) {
        return std::string("PROGRAM P VAR t, u : TIME; n : INT; r : REAL; f : BOOL; END_VAR ") + body +
               " END_PROGRAM";
    };
    CHECK(errorOf(program("t := u + T#1s - T#200ms;")) == "");
    CHECK(errorOf(program("t := u * 3; t := 2 * u; t := u / n;")) == "");
    CHECK(errorOf(program("f := t > T#5s AND t <> u;")) == "");
    CHECK(errorOf(program("t := -u;")) == "");
    CHECK(errorOf(program("t := u + 5;")) == "'+' needs two TIMEs, but got TIME and integer literal");
    CHECK(errorOf(program("t := u * r;")) == "'*' needs a TIME and an integer, but got TIME and REAL");
    CHECK(errorOf(program("t := 5 / u;")) == "'/' needs a TIME divided by an integer, but got integer literal and TIME");
    CHECK(errorOf(program("f := t > 5;")) == "'>' needs two TIMEs, but got TIME and integer literal");
    CHECK(errorOf(program("n := t;")) == "can't assign TIME to 'n', which is INT");
}

// ---------------------------------------------------------------- Running

TEST_CASE("TIME values are stored in milliseconds and wrap like a 32-bit TIME") {
    Runner runner("PROGRAM P VAR t : TIME := T#1m; u : TIME; END_VAR u := t * 2 + T#500ms; END_PROGRAM");
    runner.cycles(1);
    CHECK(runner.integer("u") == 120'500);
    CHECK(formatValue(runner.value("u"), DataType::Time) == "T#2m500ms");
}

TEST_CASE("Each instance has its own memory") {
    Runner runner(std::string(kCounterBlock) + "PROGRAM Main VAR a, b : Counter; END_VAR\n"
                                               "    a(step := 10);\n"
                                               "    b();\n"
                                               "END_PROGRAM\n");
    runner.cycles(3);
    CHECK(runner.integer("a.total") == 30);
    CHECK(runner.integer("b.total") == 3); // b uses the default step of 1
}

TEST_CASE("Inputs that aren't given keep their previous value") {
    Runner runner(std::string(kCounterBlock) + "PROGRAM Main VAR c : Counter; n : INT; END_VAR\n"
                                               "    n := n + 1;\n"
                                               "    IF n = 1 THEN c(step := 5); ELSE c(); END_IF;\n"
                                               "END_PROGRAM\n");
    runner.cycles(3);
    CHECK(runner.integer("c.total") == 15); // 5 on every call, not just the first
}

TEST_CASE("A function block only runs when it's called") {
    Runner runner(std::string(kCounterBlock) + "PROGRAM Main VAR c : Counter; n : INT; END_VAR\n"
                                               "    n := n + 1;\n"
                                               "    IF n MOD 2 = 0 THEN c(); END_IF;\n"
                                               "END_PROGRAM\n");
    runner.cycles(6);
    CHECK(runner.integer("c.total") == 3);
}

TEST_CASE("RETURN inside a function block ends only that call") {
    Runner runner("FUNCTION_BLOCK Early VAR_OUTPUT before, after : BOOL; END_VAR\n"
                  "    before := TRUE; RETURN; after := TRUE;\n"
                  "END_FUNCTION_BLOCK\n"
                  "PROGRAM Main VAR e : Early; done : BOOL; END_VAR\n"
                  "    e(); done := TRUE;\n"
                  "END_PROGRAM\n");
    runner.cycles(1);
    CHECK(runner.boolean("e.before"));
    CHECK_FALSE(runner.boolean("e.after"));
    CHECK(runner.boolean("done"));
}

TEST_CASE("TON from Structured Text follows the scan cycle's clock") {
    Runner runner("PROGRAM P VAR t : TON; END_VAR t(IN := TRUE, PT := T#100ms); END_PROGRAM");
    runner.cycles(10); // cycle 1 starts the timer; 9 more cycles = 90 ms
    CHECK_FALSE(runner.boolean("t.Q"));
    CHECK(runner.integer("t.ET") == 90);
    runner.cycles(1);
    CHECK(runner.boolean("t.Q"));
    CHECK(runner.integer("t.ET") == 100);
}

TEST_CASE("Function blocks can contain other function blocks") {
    // A Blinker: two TONs inside a user block, with two independent instances.
    Runner runner("FUNCTION_BLOCK Blinker\n"
                  "VAR_INPUT period : TIME; END_VAR\n"
                  "VAR_OUTPUT out : BOOL; END_VAR\n"
                  "VAR offTimer, onTimer : TON; END_VAR\n"
                  "    offTimer(IN := NOT onTimer.Q, PT := period);\n"
                  "    onTimer(IN := offTimer.Q, PT := period);\n"
                  "    out := offTimer.Q;\n"
                  "END_FUNCTION_BLOCK\n"
                  "PROGRAM Main\n"
                  "VAR fast, slow : Blinker; fastEdges, slowEdges : CTU; END_VAR\n"
                  "    fast(period := T#50ms);\n"
                  "    slow(period := T#200ms);\n"
                  "    fastEdges(CU := fast.out, PV := 100);\n"
                  "    slowEdges(CU := slow.out, PV := 100);\n"
                  "END_PROGRAM\n");
    runner.cycles(200); // t = 0 .. 1990 ms
    // `out` first turns on after one period (50 / 200 ms). A full blink then
    // takes 2 periods plus 2 cycles of reaction delay: 120 / 420 ms. So the
    // rising edges are at 50, 170, ... 1970 ms (17) and 200, 620, 1040, 1460,
    // 1880 ms (5).
    CHECK(runner.integer("fastEdges.CV") == 17);
    CHECK(runner.integer("slowEdges.CV") == 5);
}

TEST_CASE("valueOf reads instance members but not whole instances") {
    ProcessImage image(1, 1, 1);
    CompilationUnit unit = compileProgram("PROGRAM P VAR t : TON; END_VAR END_PROGRAM").value();
    Interpreter interpreter(std::move(unit));
    CHECK(interpreter.valueOf("t.Q", image).has_value());
    CHECK(interpreter.valueOf("T.q", image).has_value()); // case-insensitive
    CHECK_FALSE(interpreter.valueOf("t", image).has_value());
    CHECK_FALSE(interpreter.valueOf("t.Missing", image).has_value());
    CHECK_FALSE(interpreter.valueOf("missing", image).has_value());
}
