#pragma once
#include "st/diagnostic.hpp"
#include <cstdint>
#include <string>
#include <variant>

namespace softplc::st {

enum class TokenKind {
    Identifier, // motorSpeed, _tmp1
    Integer,    // 42, 1_000, 16#FF, 2#1010
    Real,       // 3.14, 1.5E3
    True,       // TRUE
    False,      // FALSE

    Assign,     // :=
    Semicolon,  // ;
    LeftParen,  // (
    RightParen, // )

    Plus,  // +
    Minus, // -
    Star,  // *
    Slash, // /
    Power, // **
    Mod,   // MOD

    And, // AND or &
    Or,  // OR
    Xor, // XOR
    Not, // NOT

    Equal,        // =
    NotEqual,     // <>
    Less,         // <
    LessEqual,    // <=
    Greater,      // >
    GreaterEqual, // >=

    EndOfFile, // always the last token
};

// A short human-readable name for error messages, e.g. "':='" or "identifier".
const char* tokenKindName(TokenKind kind);

struct Token {
    TokenKind kind = TokenKind::EndOfFile;
    std::string text; // exactly as written in the source
    SourceLocation location;
    // The numeric value of Integer and Real tokens; empty (monostate) for the rest.
    std::variant<std::monostate, std::int64_t, double> value;
};

} // namespace softplc::st
