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

TEST_CASE("Typed literals must fit their type, and are checked like variables of it") {
    CHECK(errorOf(program("small := INT#40000;")) == "INT#40000 doesn't fit in INT (-32768 to 32767)");
    CHECK(errorOf(program("small := USINT#-1;")) == "USINT#-1 doesn't fit in USINT (0 to 255)");
    CHECK(errorOf(program("small := WORD#1;")) == "can't assign WORD to 'small', which is INT (convert with WORD_TO_INT)");
    CHECK(errorOf(program("big := DINT#16#7FFF_FFFF; small := SINT#5;")) == "");
    CHECK(errorOf(program("flag := BOOL#2;")) == "BOOL# takes 0, 1, TRUE or FALSE, not 2");
    CHECK(errorOf(program("small := INT#2.5;")) == "INT# needs an integer, not 2.5");
}

TEST_CASE("Part access needs a bit string or an integer, and a part inside it") {
    const auto p = [](const char* body) {
        return errorOf(std::string("PROGRAM T VAR w : WORD; r : REAL; f : BOOL; b : BYTE; a : ARRAY[1..2] OF WORD; "
                                   "END_VAR\n") + body + "\nEND_PROGRAM");
    };
    CHECK(p("f := w.%X16;") == "'w.%X16' is out of range: a WORD has bits %X0 to %X15");
    CHECK(p("b := w.%B2;") == "'w.%B2' is out of range: a WORD has bytes %B0 to %B1");
    CHECK(p("w := w.%W0;") == "'w.%W0' is out of range: a WORD has no words inside it");
    CHECK(p("f := r.%X0;") == "bit access needs a bit string or an integer, not REAL");
    CHECK(p("f := w.%X1.%X0;") == "a part like '.%X1' must come last");
    CHECK(p("r := w.%X1;") == "can't assign BOOL to 'r', which is REAL");
    CHECK(p("b := b.%B0;") == "'b.%B0' is out of range: a BYTE has no bytes inside it");
    CHECK(p("f := a[1].%X15; a[2].%B0 := b;").empty());
}

TEST_CASE("A FOR variable can't be changed through one of its bits either") {
    CHECK(errorOf(program("FOR i := 0 TO 10 DO i.%X4 := TRUE; END_FOR;")) ==
          "'i' is the variable of a FOR loop and can't be changed inside it");
}

TEST_CASE("A literal in a bit-string operation must fit the bit string") {
    const auto p = [](const char* body) {
        return errorOf(std::string("PROGRAM T VAR w : WORD; d : DWORD; f : BOOL; END_VAR\n") + body + "\nEND_PROGRAM");
    };
    CHECK(p("d := w OR 16#10000;") == "65536 doesn't fit in WORD (0 to 65535)");
    CHECK(p("w := w XOR -1;") == "-1 doesn't fit in WORD (0 to 65535)");
    CHECK(p("f := w = 16#10000;") == "65536 doesn't fit in WORD (0 to 65535)");
    CHECK(p("w := w AND 16#FF00; f := w > 16#00FF;").empty());
}

TEST_CASE("String and character type rules") {
    const auto p = [](const char* body) {
        return errorOf(std::string("PROGRAM T VAR s : STRING; t : STRING[3]; w : WSTRING; c : CHAR; wc : WCHAR; "
                                   "f : BOOL; i : INT; END_VAR\n") + body + "\nEND_PROGRAM");
    };
    CHECK(p("t := 'abcdef';") == "'abcdef' is 6 characters, too long for STRING[3]");
    CHECK(p("w := s;") == "can't assign STRING to 'w', which is WSTRING (convert with STRING_TO_WSTRING)");
    CHECK(p("s := s + 'a';") == "'+' needs numbers, but got STRING and STRING");
    CHECK(p("s := 'ab'; t := s; s := t; w := \"x\"; c := 'a'; wc := \"b\"; f := c = 'b'; f := s < 'abc';").empty());
    CHECK(p("f := w <> \"x\"; f := wc >= \"a\"; c := CHAR#'z'; s := STRING#'q';").empty());
    CHECK(p("c := 'ab';") == "can't assign STRING to 'c', which is CHAR");
    CHECK(p("f := s = w;") == "'=' needs two texts of one kind, but got STRING and WSTRING");
    CHECK(p("CASE s OF 1: i := 1; END_CASE;") == "the CASE selector must be an integer, not STRING");
    CHECK(p("i := c;") == "can't assign CHAR to 'i', which is INT");
    CHECK(p("c := CHAR#'ab';") == "CHAR# needs one character, not 2");
    CHECK(errorOf("PROGRAM T VAR s : STRING[300]; END_VAR END_PROGRAM") == "STRING length must be 1 to 254, not 300");
    CHECK(errorOf("PROGRAM T VAR s : WSTRING[16383]; END_VAR END_PROGRAM") ==
          "WSTRING length must be 1 to 16382, not 16383");
    CHECK(errorOf("PROGRAM T VAR i : INT[3]; END_VAR END_PROGRAM") == "only STRING and WSTRING take a length, not INT");
    CHECK(errorOf("PROGRAM T VAR s : STRING[2] := 'abc'; END_VAR END_PROGRAM") ==
          "'abc' is 3 characters, too long for STRING[2]");
}

TEST_CASE("A string takes one character index") {
    const auto p = [](const char* body) {
        return errorOf(std::string("PROGRAM T VAR s : STRING; c : CHAR; r : REAL; END_VAR\n") + body + "\nEND_PROGRAM");
    };
    CHECK(p("c := s[1, 2];") == "a STRING takes one index, not 2");
    CHECK(p("c := s[r];") == "a character index must be an integer, not REAL");
    CHECK(p("c := s[1].x;") == "'s[...]' is CHAR, not a struct, so it has no field 'x'");
    CHECK(p("c := s[1];").empty());
}
