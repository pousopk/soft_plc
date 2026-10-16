#include <doctest/doctest.h>
#include "st/lexer.hpp"
#include <vector>

using namespace softplc::st;

namespace {

std::vector<TokenKind> kindsOf(std::string_view source) {
    auto tokens = tokenize(source);
    REQUIRE(tokens.has_value());
    std::vector<TokenKind> kinds;
    for (const auto& token : *tokens) {
        kinds.push_back(token.kind);
    }
    return kinds;
}

Token single(std::string_view source) {
    auto tokens = tokenize(source);
    REQUIRE(tokens.has_value());
    REQUIRE(tokens->size() == 2); // the token, then EndOfFile
    return (*tokens)[0];
}

} // namespace

TEST_CASE("An assignment becomes a sequence of tokens ending with EndOfFile") {
    using K = TokenKind;
    CHECK(kindsOf("a := b + 3 * c;") ==
          std::vector{K::Identifier, K::Assign, K::Identifier, K::Plus, K::Integer, K::Star, K::Identifier,
                      K::Semicolon, K::EndOfFile});
}

TEST_CASE("Empty input and whitespace-only input give just EndOfFile") {
    CHECK(kindsOf("") == std::vector{TokenKind::EndOfFile});
    CHECK(kindsOf("  \n\t ") == std::vector{TokenKind::EndOfFile});
}

TEST_CASE("Every operator is recognised, including two-character ones") {
    using K = TokenKind;
    CHECK(kindsOf(":= ; ( ) + - * / ** = <> < <= > >= &") ==
          std::vector{K::Assign, K::Semicolon, K::LeftParen, K::RightParen, K::Plus, K::Minus, K::Star, K::Slash,
                      K::Power, K::Equal, K::NotEqual, K::Less, K::LessEqual, K::Greater, K::GreaterEqual, K::And,
                      K::EndOfFile});
}

TEST_CASE("Keywords are case-insensitive; other names are identifiers") {
    using K = TokenKind;
    CHECK(kindsOf("TRUE false Mod and OR xor Not") ==
          std::vector{K::True, K::False, K::Mod, K::And, K::Or, K::Xor, K::Not, K::EndOfFile});
    CHECK(kindsOf("motorSpeed _tmp1 ANDY") ==
          std::vector{K::Identifier, K::Identifier, K::Identifier, K::EndOfFile});
}

TEST_CASE("Statement and declaration keywords are recognised in any case") {
    using K = TokenKind;
    CHECK(kindsOf("PROGRAM end_program Var END_VAR at") ==
          std::vector{K::Program, K::EndProgram, K::Var, K::EndVar, K::At, K::EndOfFile});
    CHECK(kindsOf("IF THEN ELSIF ELSE END_IF CASE OF END_CASE") ==
          std::vector{K::If, K::Then, K::Elsif, K::Else, K::EndIf, K::Case, K::Of, K::EndCase, K::EndOfFile});
    CHECK(kindsOf("FOR TO BY DO END_FOR WHILE END_WHILE REPEAT UNTIL END_REPEAT EXIT RETURN") ==
          std::vector{K::For, K::To, K::By, K::Do, K::EndFor, K::While, K::EndWhile, K::Repeat, K::Until,
                      K::EndRepeat, K::Exit, K::Return, K::EndOfFile});
}

TEST_CASE("Colon, comma and range symbols") {
    using K = TokenKind;
    CHECK(kindsOf("x : INT;") == std::vector{K::Identifier, K::Colon, K::Identifier, K::Semicolon, K::EndOfFile});
    CHECK(kindsOf("1, 2..5:") ==
          std::vector{K::Integer, K::Comma, K::Integer, K::DotDot, K::Integer, K::Colon, K::EndOfFile});
}

TEST_CASE("Direct addresses are parsed into an Address") {
    const auto token = single("%QX0.3");
    CHECK(token.kind == TokenKind::DirectAddress);
    CHECK(std::get<softplc::Address>(token.value) ==
          softplc::Address{softplc::Area::Output, softplc::AccessSize::Bit, 0, 3});
    CHECK(std::get<softplc::Address>(single("%MW10").value).size == softplc::AccessSize::Word);
}

TEST_CASE("Identifiers keep their original spelling") {
    CHECK(single("MotorSpeed").text == "MotorSpeed");
}

TEST_CASE("Integer literals: decimal, with separators, and based") {
    CHECK(std::get<std::int64_t>(single("42").value) == 42);
    CHECK(std::get<std::int64_t>(single("1_000_000").value) == 1'000'000);
    CHECK(std::get<std::int64_t>(single("16#FF").value) == 255);
    CHECK(std::get<std::int64_t>(single("16#ff").value) == 255);
    CHECK(std::get<std::int64_t>(single("2#1010_0101").value) == 0b1010'0101);
    CHECK(std::get<std::int64_t>(single("8#17").value) == 15);
    CHECK(single("16#FF").text == "16#FF");
}

TEST_CASE("Real literals, with and without exponent") {
    CHECK(single("3.14").kind == TokenKind::Real);
    CHECK(std::get<double>(single("3.14").value) == doctest::Approx(3.14));
    CHECK(std::get<double>(single("1.5E3").value) == doctest::Approx(1500.0));
    CHECK(std::get<double>(single("2.5e-1").value) == doctest::Approx(0.25));
    CHECK(std::get<double>(single("1_000.5").value) == doctest::Approx(1000.5));
}

TEST_CASE("Comments are skipped") {
    using K = TokenKind;
    CHECK(kindsOf("a (* a block\n comment *) := // line comment\n 1;") ==
          std::vector{K::Identifier, K::Assign, K::Integer, K::Semicolon, K::EndOfFile});
}

TEST_CASE("Tokens remember their line and column") {
    auto tokens = tokenize("a :=\n  b;");
    REQUIRE(tokens.has_value());
    CHECK((*tokens)[0].location == SourceLocation{1, 1}); // a
    CHECK((*tokens)[1].location == SourceLocation{1, 3}); // :=
    CHECK((*tokens)[2].location == SourceLocation{2, 3}); // b
    CHECK((*tokens)[3].location == SourceLocation{2, 4}); // ;
}

TEST_CASE("Lexical errors report what went wrong and where") {
    struct Case {
        const char* source;
        const char* messagePart;
        SourceLocation location;
    };
    const Case cases[] = {
        {"a := $;", "unexpected character '$'", {1, 6}},
        {"x := a.b;", "unexpected '.'", {1, 7}},
        {"x AT %Z0.0", "area must be I, Q or M", {1, 6}},
        {"x AT %IX0.9", "bit number must be 0-7", {1, 6}},
        {"(* never closed", "never closed", {1, 1}},
        {"x := 12abc;", "invalid number", {1, 6}},
        {"x := 3#12;", "base must be 2, 8 or 16", {1, 6}},
        {"x := 16#;", "needs base-16 digits", {1, 6}},
        {"x := 2#102;", "invalid number", {1, 6}},
        {"x := 99999999999999999999;", "too large", {1, 6}},
    };
    for (const auto& c : cases) {
        CAPTURE(c.source);
        auto result = tokenize(c.source);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().message.find(c.messagePart) != std::string::npos);
        CHECK(result.error().location == c.location);
    }
}
