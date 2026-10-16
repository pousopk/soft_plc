#include <doctest/doctest.h>
#include "st/parser.hpp"

using namespace softplc::st;

namespace {

// Parses an expression and returns it fully parenthesized, which shows
// exactly how the parser grouped it.
std::string grouped(std::string_view source) {
    auto expression = parseExpression(source);
    if (!expression) {
        FAIL("parse error: " << expression.error().message);
    }
    return formatExpression(**expression);
}

} // namespace

TEST_CASE("Literals and variables") {
    CHECK(grouped("42") == "42");
    CHECK(grouped("3.0") == "3.0");
    CHECK(grouped("TRUE") == "TRUE");
    CHECK(grouped("speed") == "speed");
}

TEST_CASE("Multiplication binds tighter than addition") {
    CHECK(grouped("a + b * c") == "(a + (b * c))");
    CHECK(grouped("a * b + c") == "((a * b) + c)");
    CHECK(grouped("a + b MOD c") == "(a + (b MOD c))");
}

TEST_CASE("Parentheses override precedence and disappear from the tree") {
    CHECK(grouped("(a + b) * c") == "((a + b) * c)");
    CHECK(grouped("((a))") == "a");
}

TEST_CASE("Operators of the same level group left to right") {
    CHECK(grouped("a - b - c") == "((a - b) - c)");
    CHECK(grouped("a / b * c") == "((a / b) * c)");
    CHECK(grouped("a OR b OR c") == "((a OR b) OR c)");
}

TEST_CASE("Unary operators bind tighter than everything else, including **") {
    CHECK(grouped("-a * b") == "((-a) * b)");
    CHECK(grouped("-2 ** 2") == "((-2) ** 2)");
    CHECK(grouped("a ** -b") == "(a ** (-b))");
    CHECK(grouped("NOT a AND b") == "((NOT a) AND b)");
    CHECK(grouped("- -a") == "(-(-a))");
    CHECK(grouped("+a") == "a"); // unary plus changes nothing
}

TEST_CASE("Comparisons sit between arithmetic and logic") {
    CHECK(grouped("a + 1 > b * 2") == "((a + 1) > (b * 2))");
    CHECK(grouped("a < b = c >= d") == "((a < b) = (c >= d))");
    CHECK(grouped("level > 900 AND NOT ack OR stop") == "(((level > 900) AND (NOT ack)) OR stop)");
}

TEST_CASE("AND binds tighter than XOR, which binds tighter than OR") {
    CHECK(grouped("a OR b XOR c AND d") == "(a OR (b XOR (c AND d)))");
    CHECK(grouped("a & b") == "(a AND b)");
}

TEST_CASE("parseStatements reads a sequence of assignments") {
    auto statements = parseStatements("a := 1;\n b := a + 2;");
    REQUIRE(statements.has_value());
    REQUIRE(statements->size() == 2);
    CHECK((*statements)[0].target == "a");
    CHECK(formatExpression(*(*statements)[0].value) == "1");
    CHECK((*statements)[1].target == "b");
    CHECK(formatExpression(*(*statements)[1].value) == "(a + 2)");
    CHECK((*statements)[1].location == SourceLocation{2, 2});
}

TEST_CASE("An empty program has no statements") {
    auto statements = parseStatements("  (* nothing here *)  ");
    REQUIRE(statements.has_value());
    CHECK(statements->empty());
}

TEST_CASE("dumpTree shows one node per line, indented by depth") {
    auto statements = parseStatements("a := b + 3 * c;");
    REQUIRE(statements.has_value());
    CHECK(dumpTree((*statements)[0]) == "Assignment a\n"
                                        "  Binary +\n"
                                        "    Variable b\n"
                                        "    Binary *\n"
                                        "      Integer 3\n"
                                        "      Variable c\n");
}

TEST_CASE("Syntax errors report what was expected and where") {
    struct Case {
        const char* source;
        const char* messagePart;
        SourceLocation location;
    };
    const Case cases[] = {
        {"a := b + * c;", "expected an expression, found '*'", {1, 10}},
        {"a := ;", "expected an expression, found ';'", {1, 6}},
        {"a := (b + c;", "expected ')' to close the '('", {1, 12}},
        {"a := b", "expected ';' at the end of the statement, found end of input", {1, 7}},
        {"a + b;", "expected ':=' after 'a'", {1, 3}},
        {"5 := a;", "expected a variable name", {1, 1}},
        {"a := 1;\nb := 2 3;", "expected ';'", {2, 8}},
        {"a := $;", "unexpected character", {1, 6}}, // lexer errors come through too
    };
    for (const auto& c : cases) {
        CAPTURE(c.source);
        auto result = parseStatements(c.source);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().message.find(c.messagePart) != std::string::npos);
        CHECK(result.error().location == c.location);
    }
}

TEST_CASE("parseExpression rejects anything after the expression") {
    auto result = parseExpression("a + b c");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message.find("unexpected 'c'") != std::string::npos);
}
