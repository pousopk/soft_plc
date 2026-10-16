#pragma once
#include "core/address.hpp"
#include "st/diagnostic.hpp"
#include <cstdint>
#include <string>
#include <variant>

namespace softplc::st {

enum class TokenKind {
    Identifier,    // motorSpeed, _tmp1
    Integer,       // 42, 1_000, 16#FF, 2#1010
    Real,          // 3.14, 1.5E3
    Time,          // T#500ms, TIME#1m30s (value: milliseconds)
    DirectAddress, // %IX0.0, %MW10
    True,          // TRUE
    False,         // FALSE

    Assign,     // :=
    Semicolon,  // ;
    Colon,      // :
    Comma,      // ,
    DotDot,     // ..
    Dot,        // .  (as in t1.Q)
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

    // Program structure
    Program, EndProgram, FunctionBlock, EndFunctionBlock,
    Var, VarInput, VarOutput, EndVar, At,
    // Statements
    If, Then, Elsif, Else, EndIf,
    Case, Of, EndCase,
    For, To, By, Do, EndFor,
    While, EndWhile,
    Repeat, Until, EndRepeat,
    Exit, Return,

    EndOfFile, // always the last token
};

// A short human-readable name for error messages, e.g. "':='" or "identifier".
const char* tokenKindName(TokenKind kind);

struct Token {
    TokenKind kind = TokenKind::EndOfFile;
    std::string text; // exactly as written in the source
    SourceLocation location;
    // Integer and Time tokens carry an integer (Time: milliseconds), Real
    // tokens a double, DirectAddress tokens their parsed address; the rest
    // carry nothing (monostate).
    std::variant<std::monostate, std::int64_t, double, Address> value;
};

} // namespace softplc::st
