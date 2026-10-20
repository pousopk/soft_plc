#include "st/analyzer.hpp"
#include "st/interpreter.hpp"
#include "st/parser.hpp"
#include "support/engine.hpp"
#include <chrono>
#include <doctest/doctest.h>
#include <string>

using namespace softplc;
using namespace softplc::st;

namespace {

// The error message for a program, or "" if it compiles.
std::string errorOf(const std::string& source) {
    auto result = compileProgram(source);
    return result ? std::string() : result.error().message;
}

// Compiles and runs a program, then gives access to variables by path.
class Runner {
public:
    explicit Runner(const std::string& source)
        : image_(8, 8, 64), interpreter_(compile(source, image_), 1'000'000, testEngine()) {}

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

    ProcessImage& image() { return image_; }
    Interpreter& interpreter() { return interpreter_; }

private:
    static CompilationUnit compile(const std::string& source, ProcessImage& image) {
        auto unit = compileProgram(source);
        if (!unit) {
            FAIL("compile error: " << unit.error().message);
        }
        for (const auto& [number, size] : dataBlocksUsedBy(*unit)) {
            image.addDataBlock(number, size);
        }
        return std::move(*unit);
    }

    ProcessImage image_;
    Interpreter interpreter_;
    TimePoint now_ = TimePoint{} + std::chrono::seconds(100);
};

constexpr const char* kPointTypes = R"(
TYPE
    Point : STRUCT
        x : REAL;
        y : REAL;
    END_STRUCT;
    Line : STRUCT
        start : Point;
        finish : Point := (x := 1.0, y := 1.0);
        visible : BOOL := TRUE;
    END_STRUCT;
    Row : ARRAY[1..4] OF INT;
END_TYPE
)";

std::string withTypes(const std::string& program) {
    return std::string(kPointTypes) + program;
}

} // namespace

// ---------------------------------------------------------------- Parsing

TEST_CASE("TYPE blocks, ARRAY types and initial values are parsed") {
    auto unit = parseProgram(withTypes("PROGRAM P VAR\n"
                                       "  grid : ARRAY[1..3, 0..1] OF INT := [1, 2, 4(0)];\n"
                                       "  p : Point := (x := 1.5);\n"
                                       "END_VAR\n"
                                       "  grid[i + 1, 0] := p.x;\n"
                                       "END_PROGRAM"));
    REQUIRE(unit.has_value());
    const auto tree = dumpTree(*unit);
    CHECK(tree.find("Type Point : Struct\n  x : REAL\n  y : REAL\n") != std::string::npos);
    CHECK(tree.find("  finish : Point := (x := 1.0, y := 1.0)\n") != std::string::npos);
    CHECK(tree.find("Type Row : ARRAY[1..4] OF INT\n") != std::string::npos);
    CHECK(tree.find("    grid : ARRAY[1..3, 0..1] OF INT := [1, 2, 4(0)]\n") != std::string::npos);
    CHECK(tree.find("    p : Point := (x := 1.5)\n") != std::string::npos);
    CHECK(tree.find("    Assignment grid[(i + 1), 0]\n      Access p.x\n") != std::string::npos);
}

TEST_CASE("Array and struct syntax errors") {
    struct Case {
        const char* source;
        const char* messagePart;
    };
    const Case cases[] = {
        {"PROGRAM P VAR a : ARRAY[1..] OF INT; END_VAR END_PROGRAM", "expected the upper bound of the array"},
        {"PROGRAM P VAR a : ARRAY[5..1] OF INT; END_VAR END_PROGRAM", "array bounds 5..1 are empty"},
        {"PROGRAM P VAR a : ARRAY[1..3] INT; END_VAR END_PROGRAM", "expected 'OF' after ARRAY[...]"},
        {"PROGRAM P VAR a : ARRAY[1..3] OF ARRAY[1..2] OF INT; END_VAR END_PROGRAM",
         "an array of arrays isn't allowed"},
        {"TYPE T : STRUCT END_STRUCT; END_TYPE PROGRAM P END_PROGRAM", "STRUCT 'T' has no fields"},
        {"TYPE T : STRUCT x : INT; END_STRUCT END_TYPE PROGRAM P END_PROGRAM",
         "expected ';' after the type declaration"},
        {"PROGRAM P VAR a : ARRAY[1..3] OF INT := [1, 2; END_VAR END_PROGRAM", "expected ']' to close the array's"},
        {"PROGRAM P VAR a : ARRAY[1..3] OF INT; END_VAR a[1 := 2; END_PROGRAM", "expected ']' after the array index"},
    };
    for (const auto& c : cases) {
        CAPTURE(c.source);
        auto result = parseProgram(c.source);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().message.find(c.messagePart) != std::string::npos);
    }
}

// ---------------------------------------------------------------- Checks

TEST_CASE("Array and struct rules are checked") {
    struct Case {
        std::string source;
        const char* message;
    };
    const std::string vars = "PROGRAM P VAR a : ARRAY[1..10] OF INT; g : ARRAY[1..2, 1..3] OF INT; p, q : Point; "
                             "l : Line; i : INT; r : REAL; f : BOOL; END_VAR\n";
    const Case cases[] = {
        {withTypes(vars + "p.z := 1.0; END_PROGRAM"), "Point has no field 'z'"},
        {withTypes(vars + "i := i.x; END_PROGRAM"),
         "'i' is not a function block instance or a struct, so it has no members"},
        {withTypes(vars + "r := p.x.y; END_PROGRAM"), "'p.x' is REAL, not a struct, so it has no field 'y'"},
        {withTypes(vars + "i := p[1]; END_PROGRAM"), "'p' is Point, not an array"},
        {withTypes(vars + "i := a[1, 2]; END_PROGRAM"), "'a' is ARRAY[1..10] OF INT: it needs 1 index, not 2"},
        {withTypes(vars + "i := g[1]; END_PROGRAM"), "'g' is ARRAY[1..2, 1..3] OF INT: it needs 2 indexes, not 1"},
        {withTypes(vars + "i := a[11]; END_PROGRAM"), "index 11 is outside the bounds of 'a' (1..10)"},
        {withTypes(vars + "i := a[0]; END_PROGRAM"), "index 0 is outside the bounds of 'a' (1..10)"},
        {withTypes(vars + "i := a[r]; END_PROGRAM"), "an array index must be an integer, not REAL"},
        {withTypes(vars + "i := a; END_PROGRAM"), "can't assign ARRAY[1..10] OF INT to 'i', which is INT"},
        {withTypes(vars + "p := l; END_PROGRAM"), "can't assign Line to 'p', which is Point"},
        {withTypes(vars + "p := 1.0; END_PROGRAM"), "can't assign REAL to 'p', which is Point"},
        {withTypes(vars + "f := p = q; END_PROGRAM"), "'=' needs two BOOLs, two numbers or two bit strings, but got Point and Point"},
        {withTypes(vars + "i := a + 1; END_PROGRAM"),
         "'+' needs numbers, but got ARRAY[1..10] OF INT and integer literal"},
        {withTypes(vars + "IF p THEN END_IF; END_PROGRAM"), "the IF condition must be BOOL, not Point"},
        {withTypes(vars + "a[1] := TRUE; END_PROGRAM"), "can't assign BOOL to 'a[1]', which is INT"},
        {withTypes("PROGRAM P VAR a : ARRAY[1..3] OF INT := [1, 2, 3, 4]; END_VAR END_PROGRAM"),
         "too many initial values for 'a': 4 for 3 elements"},
        {withTypes("PROGRAM P VAR a : ARRAY[1..3] OF INT := [2(1), 2(2)]; END_VAR END_PROGRAM"),
         "too many initial values for 'a': 4 for 3 elements"},
        {withTypes("PROGRAM P VAR a : ARRAY[1..3] OF INT := 5; END_VAR END_PROGRAM"),
         "'a' is ARRAY[1..3] OF INT: write its initial values as [1, 2, 3]"},
        {withTypes("PROGRAM P VAR p : Point := [1.0, 2.0]; END_VAR END_PROGRAM"),
         "'p' is Point, not an array, so it can't have [...] initial values"},
        {withTypes("PROGRAM P VAR p : Point := (z := 1.0); END_VAR END_PROGRAM"), "Point has no field 'z'"},
        {withTypes("PROGRAM P VAR p : Point := (x := TRUE); END_VAR END_PROGRAM"),
         "can't assign BOOL to 'p.x', which is REAL"},
        {withTypes("PROGRAM P VAR a : ARRAY[1..3] OF INT := [1, 99999]; END_VAR END_PROGRAM"),
         "99999 doesn't fit in INT (-32768 to 32767)"},
        {withTypes("PROGRAM P VAR i : INT; a : ARRAY[1..3] OF INT := [i]; END_VAR END_PROGRAM"),
         "initial values must be constants, not variables like 'i'"},
        {"TYPE A : STRUCT b : B; END_STRUCT; B : STRUCT a : A; END_STRUCT; END_TYPE PROGRAM P END_PROGRAM",
         "type 'A' contains itself: A -> B -> A"},
        {"TYPE T : STRUCT x : INT; X : BOOL; END_STRUCT; END_TYPE PROGRAM P END_PROGRAM",
         "STRUCT 'T' already has a field 'X'"},
        {"TYPE T : STRUCT t : TON; END_STRUCT; END_TYPE PROGRAM P END_PROGRAM",
         "'TON' is a function block: arrays and structs of function block instances aren't supported"},
        {"PROGRAM P VAR timers : ARRAY[1..3] OF TON; END_VAR END_PROGRAM",
         "'TON' is a function block: arrays and structs of function block instances aren't supported"},
        {"TYPE TON : STRUCT x : INT; END_STRUCT; END_TYPE PROGRAM P END_PROGRAM",
         "'TON' is a standard function block; choose another name"},
        {"TYPE Int : STRUCT x : INT; END_STRUCT; END_TYPE PROGRAM P END_PROGRAM",
         "'Int' is an elementary type; choose another name"},
        {"TYPE T : STRUCT x : INT; END_STRUCT; T : STRUCT y : INT; END_STRUCT; END_TYPE PROGRAM P END_PROGRAM",
         "type 'T' is already defined on line 1"},
        {"TYPE T : STRUCT x AT %MW0 : INT; END_STRUCT; END_TYPE PROGRAM P END_PROGRAM",
         "a STRUCT field can't have an AT address; place the whole variable with AT"},
        {"PROGRAM P VAR a : ARRAY[1..2000, 1..1000] OF INT; END_VAR END_PROGRAM",
         "ARRAY[1..2000, 1..1000] OF INT is too big (more than 1000000 values)"},
        {withTypes("PROGRAM P VAR p AT %MB1 : Point; END_VAR END_PROGRAM"),
         "Point 'p' needs an even byte address like %MB10 or %DB1.DBB0, not %MB1"},
        {withTypes("PROGRAM P VAR p AT %MW0 : Point; END_VAR END_PROGRAM"),
         "Point 'p' needs an even byte address like %MB10 or %DB1.DBB0, not %MW0"},
        {withTypes("PROGRAM P VAR p AT %MB0 : Point := (x := 1.0); END_VAR END_PROGRAM"),
         "'p' can't have an initial value: its value comes from %MB0"},
    };
    for (const auto& c : cases) {
        CAPTURE(c.source);
        CHECK(errorOf(c.source) == c.message);
    }
}

TEST_CASE("Correct uses of arrays and structs pass the checks") {
    const std::string vars = "PROGRAM P VAR a, b : ARRAY[1..10] OF INT; c : Row; d : ARRAY[1..4] OF INT; "
                             "p, q : Point; l : Line; i : INT; r : REAL; END_VAR\n";
    CHECK(errorOf(withTypes(vars + "a := b; p := q; l.start := p; END_PROGRAM")) == "");
    CHECK(errorOf(withTypes(vars + "c := d; d := c; END_PROGRAM")) == ""); // same dimensions and element type
    CHECK(errorOf(withTypes(vars + "a[i] := a[i + 1] * 2; r := l.finish.x + a[3]; END_PROGRAM")) == "");
    CHECK(errorOf(withTypes(vars + "FOR i := 1 TO 10 DO a[i] := i; END_FOR; END_PROGRAM")) == "");
}

// ---------------------------------------------------------------- Running

TEST_CASE("Array elements are read and written with constant and computed indices") {
    Runner run("PROGRAM P VAR a : ARRAY[1..5] OF INT; i, sum : INT; END_VAR\n"
               "  FOR i := 1 TO 5 DO a[i] := i * 10; END_FOR;\n"
               "  a[2] := a[2] + 1;\n"
               "  sum := 0;\n"
               "  FOR i := 1 TO 5 DO sum := sum + a[i]; END_FOR;\n"
               "END_PROGRAM");
    run.cycles(1);
    CHECK(run.integer("a[1]") == 10);
    CHECK(run.integer("a[2]") == 21);
    CHECK(run.integer("a[5]") == 50);
    CHECK(run.integer("sum") == 151);
}

TEST_CASE("Arrays can start at any bound, also negative") {
    Runner run("PROGRAM P VAR a : ARRAY[-2..2] OF DINT; i : INT; END_VAR\n"
               "  FOR i := -2 TO 2 DO a[i] := i * i; END_FOR;\n"
               "END_PROGRAM");
    run.cycles(1);
    CHECK(run.integer("a[-2]") == 4);
    CHECK(run.integer("a[0]") == 0);
    CHECK(run.integer("a[2]") == 4);
}

TEST_CASE("Two-dimensional arrays are stored row by row") {
    Runner run("PROGRAM P VAR g : ARRAY[1..3, 1..4] OF INT; r, c : INT; END_VAR\n"
               "  FOR r := 1 TO 3 DO FOR c := 1 TO 4 DO g[r, c] := r * 10 + c; END_FOR; END_FOR;\n"
               "END_PROGRAM");
    run.cycles(1);
    CHECK(run.integer("g[1,1]") == 11);
    CHECK(run.integer("g[2,3]") == 23);
    CHECK(run.integer("g[3,4]") == 34);
    // The listing goes through the last index first.
    const auto variables = run.interpreter().variables();
    REQUIRE(variables.size() >= 3);
    CHECK(variables[0].path == "g[1,1]");
    CHECK(variables[1].path == "g[1,2]");
    CHECK(variables[4].path == "g[2,1]");
}

TEST_CASE("Struct fields, nested structs and arrays of structs") {
    Runner run(withTypes("PROGRAM P VAR l : Line; pts : ARRAY[1..3] OF Point; i : INT; len : REAL; END_VAR\n"
                         "  l.start.x := 3.0; l.start.y := 4.0;\n"
                         "  FOR i := 1 TO 3 DO pts[i].x := i; pts[i].y := i * 2; END_FOR;\n"
                         "  len := (l.finish.x - l.start.x) ** 2;\n"
                         "END_PROGRAM"));
    run.cycles(1);
    CHECK(run.real("l.start.x") == doctest::Approx(3.0));
    CHECK(run.real("l.finish.y") == doctest::Approx(1.0)); // the TYPE's default
    CHECK(run.boolean("l.visible"));
    CHECK(run.real("pts[2].y") == doctest::Approx(4.0));
    CHECK(run.real("pts[3].x") == doctest::Approx(3.0));
    CHECK(run.real("len") == doctest::Approx(4.0));
}

TEST_CASE("Whole arrays and structs are copied, not shared") {
    Runner run(withTypes("PROGRAM P VAR a, b : ARRAY[1..3] OF INT := [1, 2, 3]; p, q : Point; l : Line; "
                         "first : BOOL := TRUE; END_VAR\n"
                         "  IF first THEN\n"
                         "    b := a; b[2] := 20;\n"
                         "    p.x := 5.0; q := p; p.x := 6.0;\n"
                         "    l.start := q;\n"
                         "    first := FALSE;\n"
                         "  END_IF;\n"
                         "END_PROGRAM"));
    run.cycles(1);
    CHECK(run.integer("a[2]") == 2);
    CHECK(run.integer("b[2]") == 20);
    CHECK(run.integer("b[3]") == 3);
    CHECK(run.real("q.x") == doctest::Approx(5.0));
    CHECK(run.real("p.x") == doctest::Approx(6.0));
    CHECK(run.real("l.start.x") == doctest::Approx(5.0));
}

TEST_CASE("Initial values: lists, repetition, struct fields and type defaults") {
    Runner run(withTypes("PROGRAM P VAR\n"
                         "  a : ARRAY[1..6] OF INT := [7, 2(5), 1];\n" // 7 5 5 1, then 0 0
                         "  t : ARRAY[0..1] OF TIME := [T#1s];\n"
                         "  p : Point := (y := 2.5);\n"
                         "  lines : ARRAY[1..2] OF Line := [(visible := FALSE)];\n"
                         "END_VAR\n"
                         "END_PROGRAM"));
    CHECK(run.integer("a[1]") == 7);
    CHECK(run.integer("a[3]") == 5);
    CHECK(run.integer("a[4]") == 1);
    CHECK(run.integer("a[6]") == 0);
    CHECK(run.integer("t[0]") == 1000);
    CHECK(run.real("p.x") == doctest::Approx(0.0));
    CHECK(run.real("p.y") == doctest::Approx(2.5));
    CHECK_FALSE(run.boolean("lines[1].visible"));
    CHECK(run.boolean("lines[2].visible"));                       // the TYPE's default
    CHECK(run.real("lines[2].finish.x") == doctest::Approx(1.0)); // the TYPE's default, inside an array
}

TEST_CASE("An index outside the array stops the PLC with a clear error") {
    Runner run("PROGRAM P VAR a : ARRAY[1..3] OF INT; i : INT := 1; END_VAR\n"
               "  i := i + 1;\n"
               "  a[i] := 5;\n"
               "END_PROGRAM");
    run.cycles(2); // i = 2, then 3: fine
    try {
        run.cycles(1); // i = 4
        FAIL("expected a runtime error");
    } catch (const RuntimeError& error) {
        CHECK(error.message == "array index 4 is outside the bounds 1..3");
        CHECK(error.location.line == 3);
        CHECK(error.location.column == 5);
    }
}

TEST_CASE("Function blocks can have array and struct inputs and outputs") {
    Runner run(withTypes("FUNCTION_BLOCK Stats\n"
                         "VAR_INPUT values : ARRAY[1..4] OF INT; END_VAR\n"
                         "VAR_OUTPUT result : STRUCT_RESULT; END_VAR\n"
                         "VAR i : INT; END_VAR\n"
                         "  result.sum := 0; result.max := values[1];\n"
                         "  FOR i := 1 TO 4 DO\n"
                         "    result.sum := result.sum + values[i];\n"
                         "    IF values[i] > result.max THEN result.max := values[i]; END_IF;\n"
                         "  END_FOR;\n"
                         "END_FUNCTION_BLOCK\n"
                         "TYPE STRUCT_RESULT : STRUCT sum : DINT; max : INT; END_STRUCT; END_TYPE\n"
                         "PROGRAM P VAR data : ARRAY[1..4] OF INT := [3, 9, -2, 5]; s : Stats; total : DINT; "
                         "copy : STRUCT_RESULT; END_VAR\n"
                         "  s(values := data);\n"
                         "  total := s.result.sum;\n"
                         "  copy := s.result;\n"
                         "END_PROGRAM"));
    run.cycles(1);
    CHECK(run.integer("total") == 15);
    CHECK(run.integer("s.result.max") == 9);
    CHECK(run.integer("copy.max") == 9);
    CHECK(run.integer("s.values[2]") == 9);
}

// ---------------------------------------------------------------- AT: the S7 layout

TEST_CASE("Structs placed with AT follow the S7 standard layout") {
    // Like a vision result UDT in a DB that a PLC reads: BOOLs packed into
    // bits, INT and REAL at even bytes, each struct a whole number of words.
    Runner run("TYPE Result : STRUCT\n"
               "    ok : BOOL;      (* DBX0.0 *)\n"
               "    nok : BOOL;     (* DBX0.1 *)\n"
               "    count : INT;    (* DBW2   *)\n"
               "    score : REAL;   (* DBD4   *)\n"
               "END_STRUCT; END_TYPE\n" // 8 bytes
               "PROGRAM P VAR results AT %DB10.DBB0 : ARRAY[1..2] OF Result; END_VAR\n"
               "  results[1].ok := TRUE; results[1].count := 258; results[1].score := 1.0;\n"
               "  results[2].nok := TRUE; results[2].count := -1;\n"
               "END_PROGRAM");
    run.cycles(1);
    auto& image = run.image();
    REQUIRE(image.dataBlock(10).size() == 16); // made just big enough
    const auto db = image.dataBlock(10);
    CHECK(db[0] == 0b01); // results[1].ok
    CHECK(db[2] == 0x01); // results[1].count = 258 = 0x0102, big-endian
    CHECK(db[3] == 0x02);
    CHECK(db[4] == 0x3F); // results[1].score = 1.0f = 0x3F800000
    CHECK(db[5] == 0x80);
    CHECK(db[8] == 0b10);  // results[2].nok
    CHECK(db[10] == 0xFF); // results[2].count = -1
    CHECK(db[11] == 0xFF);
    // And back: when an S7 client writes the DB, the program reads the new value.
    db[10] = 0x01;
    db[11] = 0x2C; // 300
    CHECK(run.integer("results[2].count") == 300);
    CHECK(run.real("results[1].score") == doctest::Approx(1.0));
}

TEST_CASE("BOOL arrays placed with AT are packed bit after bit") {
    Runner run("PROGRAM P VAR flags AT %MB4 : ARRAY[0..9] OF BOOL; i : INT; END_VAR\n"
               "  FOR i := 0 TO 9 DO flags[i] := i MOD 3 = 0; END_FOR;\n"
               "END_PROGRAM");
    run.cycles(1);
    // flags[0], [3], [6] in %MB4 (bits 0, 3, 6), flags[9] is %M5.1.
    CHECK(run.image().readByte(Area::Marker, 4) == 0b0100'1001);
    CHECK(run.image().readByte(Area::Marker, 5) == 0b0000'0010);
}

TEST_CASE("Located arrays of INT use consecutive words") {
    Runner run("PROGRAM P VAR w AT %MB10 : ARRAY[1..3] OF INT; i : INT; END_VAR\n"
               "  FOR i := 1 TO 3 DO w[i] := i * 100; END_FOR;\n"
               "END_PROGRAM");
    run.cycles(1);
    CHECK(run.image().readWord(Area::Marker, 10) == 100);
    CHECK(run.image().readWord(Area::Marker, 12) == 200);
    CHECK(run.image().readWord(Area::Marker, 14) == 300);
}

// ---------------------------------------------------------------- Monitoring

TEST_CASE("Monitoring sees every array element and struct field by path") {
    Runner run(withTypes("PROGRAM P VAR l : Line; a : ARRAY[1..2] OF INT; END_VAR END_PROGRAM"));
    const auto variables = run.interpreter().variables();
    std::vector<std::string> paths;
    for (const auto& v : variables) {
        paths.push_back(v.path);
    }
    CHECK(paths ==
          std::vector<std::string>{"l.start.x", "l.start.y", "l.finish.x", "l.finish.y", "l.visible", "a[1]", "a[2]"});

    // Paths are case-insensitive and come back spelled as declared.
    const auto info = run.interpreter().describe("L.START.X");
    REQUIRE(info.has_value());
    CHECK(info->path == "l.start.x");
    CHECK(info->type == DataType::Real);

    CHECK(run.interpreter().setValue("a[2]", std::int64_t{42}, run.image()));
    CHECK(run.integer("a[2]") == 42);

    // A whole struct, a missing field or an index out of range is no single value.
    CHECK_FALSE(run.interpreter().describe("l.start").has_value());
    CHECK_FALSE(run.interpreter().describe("l.middle").has_value());
    CHECK_FALSE(run.interpreter().describe("a[3]").has_value());
    CHECK_FALSE(run.interpreter().describe("a").has_value());
}

TEST_CASE("Accesses with a fixed place get a direct leaf, the rest don't") {
    auto unit = compileProgram("PROGRAM P VAR a : INT; b : ARRAY[1..3] OF INT; i : INT; m AT %MW0 : INT; END_VAR\n"
                               "  a := b[2]; a := b[i]; a := m;\n"
                               "END_PROGRAM");
    REQUIRE(unit.has_value());
    const auto& body = unit->program.body;
    const auto& first = std::get<Assignment>(body[0].node);
    CHECK(first.target.directLeaf == 0);
    CHECK(std::get<Access>(first.value->node).directLeaf == 2); // a is leaf 0, b[1] leaf 1, b[2] leaf 2
    CHECK(std::get<Access>(std::get<Assignment>(body[1].node).value->node).directLeaf == kNoSlot); // run-time index
    CHECK(std::get<Access>(std::get<Assignment>(body[2].node).value->node).directLeaf == kNoSlot); // AT variable
}

TEST_CASE("8-bit struct fields start at the next byte, the rest at the next even byte") {
    auto unit = compileProgram("TYPE S : STRUCT a : BOOL; b : BYTE; c : USINT; d : INT; e : LINT; END_STRUCT; END_TYPE\n"
                               "PROGRAM P VAR s AT %DB1.DBB0 : S; END_VAR END_PROGRAM");
    REQUIRE(unit.has_value());
    const auto& fields = unit->program.variables[0].typeInfo->fields;
    REQUIRE(fields.size() == 5);
    CHECK(fields[0].bitOffset == 0);
    CHECK(fields[1].bitOffset == 8);
    CHECK(fields[2].bitOffset == 16);
    CHECK(fields[3].bitOffset == 32);
    CHECK(fields[4].bitOffset == 48);
    CHECK(unit->program.variables[0].typeInfo->bitSize == 14 * 8);
    CHECK(dataBlocksUsedBy(*unit).at(1) == 14);
}

TEST_CASE("The new types take AT addresses of their size") {
    CHECK(errorOf("PROGRAM P VAR a AT %MW0 : UINT; b AT %MB2 : SINT; c AT %MD4 : DWORD; d AT %QB0 : BYTE; "
                  "e AT %DB1.DBW0 : WORD; f AT %ID0 : UDINT; END_VAR END_PROGRAM") == "");
    CHECK(errorOf("PROGRAM P VAR a AT %MD0 : LINT; END_VAR END_PROGRAM") ==
          "LINT can't be placed with AT (there's no 8-byte address); put it in a struct in a data block");
    CHECK(errorOf("PROGRAM P VAR a AT %MW0 : BYTE; END_VAR END_PROGRAM") ==
          "BYTE 'a' needs a byte address like %MB0, not %MW0");
}
