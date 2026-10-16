#include <doctest/doctest.h>
#include "st/parser.hpp"

using namespace softplc::st;

// Statements and programs are checked through dumpTree(), which shows the
// whole structure the parser built.

namespace {

std::string tree(std::string_view source) {
    auto statements = parseStatements(source);
    if (!statements) {
        FAIL("parse error: " << statements.error().message);
    }
    std::string out;
    for (const auto& statement : *statements) {
        out += dumpTree(statement);
    }
    return out;
}

} // namespace

TEST_CASE("IF with ELSIF and ELSE") {
    CHECK(tree("IF a THEN x := 1; ELSIF b THEN x := 2; ELSE x := 3; END_IF;") == "If\n"
                                                                                   "  Condition\n"
                                                                                   "    Variable a\n"
                                                                                   "  Then\n"
                                                                                   "    Assignment x\n"
                                                                                   "      Integer 1\n"
                                                                                   "  ElsifCondition\n"
                                                                                   "    Variable b\n"
                                                                                   "  Then\n"
                                                                                   "    Assignment x\n"
                                                                                   "      Integer 2\n"
                                                                                   "  Else\n"
                                                                                   "    Assignment x\n"
                                                                                   "      Integer 3\n");
}

TEST_CASE("CASE with lists, ranges, negative labels and ELSE") {
    CHECK(tree("CASE s OF 1: x := 1; 2, 4..6: x := 2; -1: x := 3; ELSE x := 0; END_CASE;") ==
          "Case\n"
          "  Selector\n"
          "    Variable s\n"
          "  Labels 1\n"
          "    Assignment x\n"
          "      Integer 1\n"
          "  Labels 2, 4..6\n"
          "    Assignment x\n"
          "      Integer 2\n"
          "  Labels -1\n"
          "    Assignment x\n"
          "      Integer 3\n"
          "  Else\n"
          "    Assignment x\n"
          "      Integer 0\n");
}

TEST_CASE("A CASE branch can hold several statements") {
    CHECK(tree("CASE s OF 1: a := 1; b := 2; END_CASE;") == "Case\n"
                                                            "  Selector\n"
                                                            "    Variable s\n"
                                                            "  Labels 1\n"
                                                            "    Assignment a\n"
                                                            "      Integer 1\n"
                                                            "    Assignment b\n"
                                                            "      Integer 2\n");
}

TEST_CASE("FOR with and without BY") {
    CHECK(tree("FOR i := 1 TO 10 DO x := i; END_FOR;") == "For i\n"
                                                          "  From\n"
                                                          "    Integer 1\n"
                                                          "  To\n"
                                                          "    Integer 10\n"
                                                          "  Do\n"
                                                          "    Assignment x\n"
                                                          "      Variable i\n");
    CHECK(tree("FOR i := 10 TO 0 BY -2 DO END_FOR;").find("  By\n    Unary -\n      Integer 2\n") !=
          std::string::npos);
}

TEST_CASE("WHILE, REPEAT, EXIT and RETURN") {
    CHECK(tree("WHILE a DO EXIT; END_WHILE;") == "While\n"
                                                 "  Condition\n"
                                                 "    Variable a\n"
                                                 "  Do\n"
                                                 "    Exit\n");
    CHECK(tree("REPEAT RETURN; UNTIL a END_REPEAT;") == "Repeat\n"
                                                         "  Do\n"
                                                         "    Return\n"
                                                         "  Until\n"
                                                         "    Variable a\n");
}

TEST_CASE("Statements nest inside each other") {
    CHECK(tree("IF a THEN WHILE b DO x := 1; END_WHILE; END_IF;") == "If\n"
                                                                      "  Condition\n"
                                                                      "    Variable a\n"
                                                                      "  Then\n"
                                                                      "    While\n"
                                                                      "      Condition\n"
                                                                      "        Variable b\n"
                                                                      "      Do\n"
                                                                      "        Assignment x\n"
                                                                      "          Integer 1\n");
}

TEST_CASE("Empty statements (lone semicolons) are skipped") {
    CHECK(tree(";; x := 1;;") == "Assignment x\n  Integer 1\n");
}

TEST_CASE("A program with variable declarations") {
    auto program = parseProgram("PROGRAM Main\n"
                                "VAR\n"
                                "    a, b : INT := 5;\n"
                                "    lamp AT %QX0.0 : BOOL;\n"
                                "    speed : real;\n"
                                "END_VAR\n"
                                "VAR\n"
                                "    total : DINT;\n"
                                "END_VAR\n"
                                "    a := b;\n"
                                "END_PROGRAM\n");
    REQUIRE(program.has_value());
    CHECK(dumpTree(*program) == "Program Main\n"
                                "  Var\n"
                                "    a : INT := 5\n"
                                "    b : INT := 5\n"
                                "    lamp AT %QX0.0 : BOOL\n"
                                "    speed : REAL\n"
                                "    total : DINT\n"
                                "  Body\n"
                                "    Assignment a\n"
                                "      Variable b\n");
    CHECK(program->program.variables[1].location == SourceLocation{3, 8});
}

TEST_CASE("Statement syntax errors") {
    struct Case {
        const char* source;
        const char* messagePart;
    };
    const Case cases[] = {
        {"IF a x := 1; END_IF;", "expected 'THEN' after the condition, found 'x'"},
        {"IF a THEN x := 1;", "expected 'END_IF' to close the IF, found end of input"},
        {"IF a THEN x := 1; END_IF", "expected ';' after END_IF"},
        {"CASE s OF x := 1; END_CASE;", "expected a case label like '1:'"},
        {"CASE s OF 1 x := 1; END_CASE;", "expected ':' after the case label"},
        {"CASE s OF 5..1: x := 1; END_CASE;", "range 5..1 is empty"},
        {"FOR i = 1 TO 10 DO END_FOR;", "expected ':=' after the loop variable"},
        {"FOR i := 1 10 DO END_FOR;", "expected 'TO' in the FOR loop"},
        {"WHILE a x := 1; END_WHILE;", "expected 'DO' after the WHILE condition"},
        {"REPEAT x := 1; END_REPEAT;", "expected 'UNTIL'"},
        {"EXIT", "expected ';' after EXIT"},
        {"END_IF;", "expected a statement, found 'END_IF'"},
    };
    for (const auto& c : cases) {
        CAPTURE(c.source);
        auto result = parseStatements(c.source);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().message.find(c.messagePart) != std::string::npos);
    }
}

TEST_CASE("Program syntax errors") {
    struct Case {
        const char* source;
        const char* messagePart;
    };
    const Case cases[] = {
        {"x := 1;", "expected 'PROGRAM' or 'FUNCTION_BLOCK', found 'x'"},
        {"", "expected a PROGRAM"},
        {"PROGRAM A END_PROGRAM PROGRAM B END_PROGRAM", "a file can only have one PROGRAM"},
        {"PROGRAM VAR END_VAR END_PROGRAM", "expected the program name"},
        {"PROGRAM P VAR x INT; END_VAR END_PROGRAM", "expected ':' before the type"},
        {"PROGRAM P VAR x : INT END_VAR END_PROGRAM", "expected ';' at the end of the declaration"},
        {"PROGRAM P VAR a, b AT %MW0 : INT; END_VAR END_PROGRAM", "AT can only be used with a single variable"},
        {"PROGRAM P VAR x AT y : INT; END_VAR END_PROGRAM", "expected an address like %IX0.0 after AT"},
        {"PROGRAM P VAR 5 END_VAR END_PROGRAM", "expected a variable name or 'END_VAR'"},
        {"PROGRAM P x := 1;", "expected 'END_PROGRAM'"},
        {"PROGRAM P END_PROGRAM x", "expected 'PROGRAM' or 'FUNCTION_BLOCK', found 'x'"},
    };
    for (const auto& c : cases) {
        CAPTURE(c.source);
        auto result = parseProgram(c.source);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().message.find(c.messagePart) != std::string::npos);
    }
}
