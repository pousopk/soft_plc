#include <doctest/doctest.h>
#include "st/analyzer.hpp"
#include "st/interpreter.hpp"
#include "support/engine.hpp"
#include <chrono>
#include <string>

using namespace softplc;
using namespace softplc::st;

REGISTER_EXCEPTION_TRANSLATOR(const RuntimeError& error) { return doctest::String(error.message.c_str()); }

namespace {

// Runs `body` once in a program with texts s, t (STRING), w (WSTRING),
// short (STRING[4]) and an INT n, and gives back the interpreter's view.
struct Run {
    explicit Run(const std::string& body)
        : image(8, 8, 64),
          interpreter(compile("PROGRAM P VAR s, t : STRING; w : WSTRING; short : STRING[4]; n : INT; c : CHAR; "
                              "u : USINT; END_VAR\n" + body + "\nEND_PROGRAM"),
                      1'000'000, testEngine()) {
        interpreter.runCycle(image, TimePoint{} + std::chrono::seconds(1));
    }

    static CompilationUnit compile(const std::string& source) {
        auto unit = compileProgram(source);
        if (!unit) {
            FAIL("compile error: " << unit.error().message);
        }
        return std::move(*unit);
    }

    std::u16string text(std::string_view path) const { return *interpreter.textOf(path, image); }
    std::int64_t integer(std::string_view path) const { return std::get<std::int64_t>(*interpreter.valueOf(path, image)); }

    ProcessImage image;
    Interpreter interpreter;
};

std::u16string textOf(const std::string& expression) { return Run("s := " + expression + ";").text("s"); }
std::int64_t integerOf(const std::string& expression) { return Run("n := " + expression + ";").integer("n"); }

std::string errorOf(const std::string& body) {
    auto unit = compileProgram("PROGRAM P VAR s : STRING; w : WSTRING; n : INT; END_VAR\n" + body + "\nEND_PROGRAM");
    return unit ? std::string() : unit.error().message;
}

} // namespace

TEST_CASE("LEN, LEFT, RIGHT and MID") {
    CHECK(integerOf("LEN('hello')") == 5);
    CHECK(integerOf("LEN('')") == 0);
    CHECK(textOf("LEFT('hello', 2)") == u"he");
    CHECK(textOf("LEFT(IN := 'hello', L := 0)") == u"");
    CHECK(textOf("RIGHT('hello', 3)") == u"llo");
    CHECK(textOf("MID('hello', 3, 2)") == u"ell");
    CHECK(textOf("MID(IN := 'hello', L := 1, P := 5)") == u"o");
}

TEST_CASE("CONCAT, INSERT, DELETE, REPLACE and FIND") {
    CHECK(textOf("CONCAT('a', 'b', 'c')") == u"abc");
    CHECK(textOf("INSERT('helo', 'l', 3)") == u"hello");
    CHECK(textOf("INSERT('ab', 'x', 0)") == u"xab");
    CHECK(textOf("INSERT('ab', 'x', 2)") == u"abx");
    CHECK(textOf("DELETE('hello', 2, 2)") == u"hlo");
    CHECK(textOf("REPLACE('hello', 'J', 1, 1)") == u"Jello");
    CHECK(textOf("REPLACE(IN1 := 'hello', IN2 := 'ipp', L := 2, P := 3)") == u"heippo");
    CHECK(integerOf("FIND('hello', 'll')") == 3);
    CHECK(integerOf("FIND('hello', 'z')") == 0);
    CHECK(integerOf("FIND(IN1 := 'abab', IN2 := 'b')") == 2);
}

TEST_CASE("The string functions work on WSTRINGs too") {
    Run r("w := CONCAT(\"$20AC\", \"1\"); n := LEN(w); w := MID(w, 1, 2);");
    CHECK(r.text("w") == u"1");
    CHECK(r.integer("n") == 2);
}

TEST_CASE("Results are cut to fit, and a string may be used on both sides") {
    Run r("s := 'ab'; s := CONCAT(s, s); t := MID(s, 3, 2); short := CONCAT(s, s);");
    CHECK(r.text("s") == u"abab");
    CHECK(r.text("t") == u"bab");
    CHECK(r.text("short") == u"abab");
    Run cut("short := CONCAT('abc', 'def');");
    CHECK(cut.text("short") == u"abcd");
}

TEST_CASE("A position or length outside the string stops the PLC") {
    CHECK_THROWS_WITH(Run("s := MID('hello', 3, 4);"), "MID: L = 3 from P = 4 goes past the end of a text of 5");
    CHECK_THROWS_WITH(Run("s := MID('hello', 1, 0);"), "MID: P = 0 is outside 1..5");
    CHECK_THROWS_WITH(Run("s := LEFT('hello', -1);"), "LEFT: L = -1 is outside 0..5");
    CHECK_THROWS_WITH(Run("s := RIGHT('hello', 6);"), "RIGHT: L = 6 is outside 0..5");
    CHECK_THROWS_WITH(Run("s := INSERT('ab', 'x', 3);"), "INSERT: P = 3 is outside 0..2");
    CHECK_THROWS_WITH(Run("s := DELETE('hello', 2, 5);"), "DELETE: L = 2 from P = 5 goes past the end of a text of 5");
}

TEST_CASE("String function arguments are checked") {
    CHECK(errorOf("s := CONCAT('a', \"b\");") == "CONCAT needs texts of one kind, but got STRING and WSTRING");
    CHECK(errorOf("n := LEN(5);") == "LEN needs a STRING or WSTRING for IN, not integer literal");
    CHECK(errorOf("s := LEFT('a', 1.5);") == "LEFT needs an integer for L, not REAL");
    CHECK(errorOf("s := CONCAT('a');") == "CONCAT needs at least 2 arguments");
    CHECK(errorOf("s := MID('a', 1);") == "MID needs 3 arguments, not 2");
    CHECK(errorOf("w := LEFT(\"abc\", 1); n := FIND(w, \"b\");").empty());
}

TEST_CASE("Values convert to text as the monitor shows them") {
    CHECK(textOf("INT_TO_STRING(-5)") == u"-5");
    CHECK(textOf("WORD_TO_STRING(WORD#255)") == u"16#00FF");
    CHECK(textOf("REAL_TO_STRING(2.5)") == u"2.5");
    CHECK(textOf("TIME_TO_STRING(T#1s500ms)") == u"T#1s500ms");
    CHECK(textOf("BOOL_TO_STRING(TRUE)") == u"TRUE");
    CHECK(textOf("LINT_TO_STRING(LINT#-9223372036854775807)") == u"-9223372036854775807");
    CHECK(Run("w := UDINT_TO_WSTRING(UDINT#4000000000);").text("w") == u"4000000000");
}

TEST_CASE("Text converts to values, and text that isn't one stops the PLC") {
    CHECK(integerOf("STRING_TO_INT('42')") == 42);
    CHECK(integerOf("STRING_TO_INT(' -7 ')") == -7);
    CHECK(integerOf("REAL_TO_INT(STRING_TO_REAL('1.5'))") == 2);
    CHECK(integerOf("DINT_TO_INT(TIME_TO_DINT(STRING_TO_TIME('T#2s')))") == 2000);
    CHECK(integerOf("WORD_TO_INT(WSTRING_TO_WORD(\"16#00FF\"))") == 255);
    CHECK_THROWS_WITH(Run("n := STRING_TO_INT('x');"), "STRING_TO_INT: 'x' is not an INT");
    CHECK_THROWS_WITH(Run("n := STRING_TO_INT('70000');"), "STRING_TO_INT: '70000' is not an INT");
}

TEST_CASE("Characters and texts convert into each other") {
    CHECK(Run("w := STRING_TO_WSTRING('ab');").text("w") == u"ab");
    CHECK(Run("s := WSTRING_TO_STRING(\"A$20AC\");").text("s") == u"A?");
    CHECK(Run("u := CHAR_TO_USINT('A');").integer("u") == 65);
    CHECK(Run("c := USINT_TO_CHAR(66);").text("c") == u"B");
    CHECK(Run("c := STRING_TO_CHAR('');").integer("c") == 0);
    CHECK(Run("c := STRING_TO_CHAR('xyz');").text("c") == u"x");
    CHECK(Run("s := CHAR_TO_STRING('q');").text("s") == u"q");
    CHECK(Run("c := WCHAR_TO_CHAR(WCHAR#\"$20AC\");").text("c") == u"?");
    CHECK(Run("c := WCHAR_TO_CHAR(WCHAR#\"A\");").text("c") == u"A");
    CHECK(Run("c := BYTE_TO_CHAR(BYTE#16#41);").text("c") == u"A");
}
