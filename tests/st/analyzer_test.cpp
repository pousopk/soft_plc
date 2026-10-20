#include <doctest/doctest.h>
#include "st/analyzer.hpp"
#include <string>

using namespace softplc::st;

namespace {

// Wraps a body in a program with a standard set of variables.
std::string program(std::string_view body) {
    return std::string("PROGRAM Test\n"
                       "VAR\n"
                       "    flag, other : BOOL;\n"
                       "    small : INT;\n"
                       "    big : DINT;\n"
                       "    real1 : REAL;\n"
                       "    i : INT;\n"
                       "END_VAR\n") +
           std::string(body) + "\nEND_PROGRAM\n";
}

// Returns the analyzer's error message, or "" if the program is fine.
std::string errorOf(const std::string& source) {
    auto result = compileProgram(source);
    return result ? std::string() : result.error().message;
}

} // namespace

TEST_CASE("A correct program passes and gets types and slots filled in") {
    auto result = compileProgram(program("small := small + 1;\nreal1 := small * 2.5;"));
    REQUIRE(result.has_value());

    const auto& first = std::get<Assignment>(result->program.body[0].node);
    CHECK(first.target.slot == 2); // flag, other, small
    CHECK(first.value->type == DataType::Int);

    const auto& second = std::get<Assignment>(result->program.body[1].node);
    CHECK(second.value->type == DataType::Real); // INT * REAL is REAL
}

TEST_CASE("Names are case-insensitive") {
    CHECK(errorOf(program("SMALL := Small + 1;")) == "");
}

TEST_CASE("Mixed numbers are widened to the wider type") {
    CHECK(errorOf(program("big := small + big;")) == "");
    CHECK(errorOf(program("real1 := big / 3;")) == "");
    CHECK(errorOf(program("flag := small < real1;")) == "");
    CHECK(errorOf(program("big := small;")) == "");   // INT fits in DINT
    CHECK(errorOf(program("real1 := small;")) == ""); // any number fits in REAL
}

TEST_CASE("Assignments that could lose data are rejected") {
    CHECK(errorOf(program("small := big;")) == "can't assign DINT to 'small', which is INT (values might not fit)");
    CHECK(errorOf(program("small := real1;")) == "can't assign REAL to 'small', which is INT (values might not fit)");
    CHECK(errorOf(program("flag := 1;")) == "can't assign integer literal to 'flag', which is BOOL");
    CHECK(errorOf(program("small := TRUE;")) == "can't assign BOOL to 'small', which is INT");
}

TEST_CASE("Integer literals must fit the variable") {
    CHECK(errorOf(program("small := 32767;")) == "");
    CHECK(errorOf(program("small := -32768;")) == "");
    CHECK(errorOf(program("small := 32768;")) == "32768 doesn't fit in INT (-32768 to 32767)");
    CHECK(errorOf(program("big := 3000000000;")) == "3000000000 doesn't fit in DINT (-2147483648 to 2147483647)");
}

TEST_CASE("Operators need operands of the right type") {
    CHECK(errorOf(program("small := flag + 1;")) == "'+' needs numbers, but got BOOL and integer literal");
    CHECK(errorOf(program("flag := flag AND small;")) == "'AND' needs BOOLs or bit strings, but got BOOL and INT");
    CHECK(errorOf(program("small := real1 MOD 2;")) == "'MOD' needs integers, but got REAL and integer literal");
    CHECK(errorOf(program("flag := flag = small;")) == "'=' needs two BOOLs, two numbers or two bit strings, but got BOOL and INT");
    CHECK(errorOf(program("flag := NOT small;")) == "NOT needs a BOOL or a bit string, not INT");
    CHECK(errorOf(program("small := -flag;")) == "'-' needs a number, not BOOL");
}

TEST_CASE("** always gives a REAL") {
    CHECK(errorOf(program("real1 := 2 ** 3;")) == "");
    CHECK(errorOf(program("small := 2 ** 3;")) == "can't assign REAL to 'small', which is INT (values might not fit)");
}

TEST_CASE("Conditions must be BOOL") {
    CHECK(errorOf(program("IF small THEN END_IF;")) == "the IF condition must be BOOL, not INT");
    CHECK(errorOf(program("WHILE 1 DO END_WHILE;")) == "the WHILE condition must be BOOL, not integer literal");
    CHECK(errorOf(program("REPEAT UNTIL real1 END_REPEAT;")) == "the UNTIL condition must be BOOL, not REAL");
}

TEST_CASE("CASE needs an integer selector and unique labels") {
    CHECK(errorOf(program("CASE real1 OF 1: ; END_CASE;")) == "the CASE selector must be an integer, not REAL");
    CHECK(errorOf(program("CASE small OF 1: ; 1: ; END_CASE;")) == "case label 1 is used more than once");
    CHECK(errorOf(program("CASE small OF 1..5: ; 4..9: ; END_CASE;")) == "case label 4 is used more than once");
    CHECK(errorOf(program("CASE small OF 1..5: ; 6..9: ; END_CASE;")) == "");
}

TEST_CASE("FOR loops need an integer variable that isn't changed inside the loop") {
    CHECK(errorOf(program("FOR i := 1 TO 10 DO small := i; END_FOR;")) == "");
    CHECK(errorOf(program("FOR real1 := 1 TO 10 DO END_FOR;")) == "the FOR variable 'real1' must be an integer, not REAL");
    CHECK(errorOf(program("FOR i := 1 TO real1 DO END_FOR;")) == "a FOR loop bound must be an integer, not REAL");
    CHECK(errorOf(program("FOR i := 1 TO 10 DO i := 5; END_FOR;")) ==
          "'i' is the variable of a FOR loop and can't be changed inside it");
    CHECK(errorOf(program("FOR i := 1 TO 10 DO END_FOR; i := 5;")) == ""); // fine after the loop
}

TEST_CASE("EXIT only inside loops") {
    CHECK(errorOf(program("EXIT;")) == "EXIT can only be used inside FOR, WHILE or REPEAT");
    CHECK(errorOf(program("IF flag THEN EXIT; END_IF;")) == "EXIT can only be used inside FOR, WHILE or REPEAT");
    CHECK(errorOf(program("WHILE flag DO IF other THEN EXIT; END_IF; END_WHILE;")) == "");
}

TEST_CASE("Unknown type names") {
    CHECK(errorOf("PROGRAM P VAR x : LWORD; END_VAR END_PROGRAM") ==
          "unknown type 'LWORD' (not an elementary type, a TYPE or a function block)");
}

TEST_CASE("Undeclared and duplicate variables") {
    CHECK(errorOf(program("missing := 1;")) == "'missing' is not declared (add it to a VAR block)");
    CHECK(errorOf(program("small := missing;")) == "'missing' is not declared (add it to a VAR block)");
    CHECK(errorOf("PROGRAM P VAR x : INT; X : BOOL; END_VAR END_PROGRAM") == "'X' is already declared on line 1");
}

TEST_CASE("Initial values must be constants of a compatible type") {
    CHECK(errorOf("PROGRAM P VAR x : INT := 2 * 3 + 1; END_VAR END_PROGRAM") == "");
    CHECK(errorOf("PROGRAM P VAR x : INT := 1; y : INT := x; END_VAR END_PROGRAM") ==
          "initial values must be constants, not variables like 'x'");
    CHECK(errorOf("PROGRAM P VAR x : BOOL := 1; END_VAR END_PROGRAM") ==
          "can't assign integer literal to 'x', which is BOOL");
}

TEST_CASE("AT addresses must match the variable size") {
    CHECK(errorOf("PROGRAM P VAR a AT %IX0.0 : BOOL; b AT %QW2 : INT; c AT %MD4 : DINT; d AT %MD8 : REAL; "
                  "END_VAR END_PROGRAM") == "");
    CHECK(errorOf("PROGRAM P VAR a AT %MW0 : BOOL; END_VAR END_PROGRAM") ==
          "BOOL 'a' needs a bit address like %IX0.0, not %MW0");
    CHECK(errorOf("PROGRAM P VAR a AT %IX0.0 : INT; END_VAR END_PROGRAM") ==
          "INT 'a' needs a word address like %MW0, not %IX0.0");
    CHECK(errorOf("PROGRAM P VAR a AT %QX0.0 : BOOL := TRUE; END_VAR END_PROGRAM") ==
          "'a' can't have an initial value: its value comes from %QX0.0");
}

TEST_CASE("Analyzer errors point at the problem") {
    auto result = compileProgram("PROGRAM P\nVAR x : INT; END_VAR\n  x := TRUE;\nEND_PROGRAM");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().location == SourceLocation{3, 8});
}

TEST_CASE("Bit strings and integers don't mix without a conversion") {
    const auto p = [](const char* body) {
        return errorOf(std::string("PROGRAM T VAR i : INT; w : WORD; b : BYTE; u : UINT; d : DINT; f : BOOL; END_VAR\n") +
                       body + "\nEND_PROGRAM");
    };
    CHECK(p("i := w;") == "can't assign WORD to 'i', which is INT (convert with WORD_TO_INT)");
    CHECK(p("w := i;") == "can't assign INT to 'w', which is WORD (convert with INT_TO_WORD)");
    CHECK(p("w := w + 1;") == "'+' needs numbers, but got WORD and integer literal");
    CHECK(p("i := i AND 3;") == "'AND' needs BOOLs or bit strings, but got INT and integer literal");
    CHECK(p("i := -w;") == "'-' needs a number, not WORD");
    CHECK(p("w := w AND b;").empty()); // BYTE widens to WORD
    CHECK(p("w := w OR f;").empty());  // BOOL widens to WORD
    CHECK(p("w := NOT w; f := w = b; f := w > 16#FF;").empty());
    CHECK(p("w := 16#FF00;").empty());
    CHECK(p("b := 256;") == "256 doesn't fit in BYTE (0 to 255)");
    CHECK(p("u := -1;") == "-1 doesn't fit in UINT (0 to 65535)");
    CHECK(p("d := i + u;").empty()); // INT + UINT is DINT
    CHECK(p("u := i;") == "can't assign INT to 'u', which is UINT (values might not fit)");
    CHECK(p("FOR u := 1 TO 10 DO END_FOR;").empty());
    CHECK(p("FOR w := 1 TO 10 DO END_FOR;") == "the FOR variable 'w' must be an integer, not WORD");
    CHECK(p("CASE w OF 1: i := 1; END_CASE;") == "the CASE selector must be an integer, not WORD");
    CHECK(p("w := MAX(w, b); w := LIMIT(b, w, 16#FF); w := SEL(f, w, b); w := MUX(u, w, b);").empty());
}

TEST_CASE("Integers widen only where no value can be lost") {
    const auto p = [](const char* body) {
        return errorOf(std::string("PROGRAM T VAR si : SINT; us : USINT; i : INT; ui : UINT; d : DINT; ud : UDINT; "
                                   "l : LINT; r : REAL; END_VAR\n") +
                       body + "\nEND_PROGRAM");
    };
    CHECK(p("i := si; i := us; ui := us; d := ui; l := ud; l := d; r := l; r := ud;").empty());
    CHECK(p("us := si;") == "can't assign SINT to 'us', which is USINT (values might not fit)");
    CHECK(p("ud := d;") == "can't assign DINT to 'ud', which is UDINT (values might not fit)");
    CHECK(p("l := d + ud;").empty());  // DINT + UDINT is LINT
    CHECK(p("d := d + ud;") == "can't assign LINT to 'd', which is DINT (values might not fit)");
    CHECK(p("l := 9223372036854775807;").empty());
}
