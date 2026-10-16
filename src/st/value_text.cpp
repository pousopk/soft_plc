#include "st/value_text.hpp"
#include "st/lexer.hpp"
#include "util/text.hpp"
#include <fmt/format.h>

namespace softplc::st {

namespace {

// Reuses the Structured Text lexer, so values are written exactly like in a
// program: an optional minus, then one literal.
tl::expected<Token, std::string> singleLiteral(std::string_view text, bool& negative) {
    auto tokens = tokenize(text);
    if (!tokens) {
        return tl::unexpected(tokens.error().message);
    }
    std::size_t index = 0;
    negative = (*tokens)[0].kind == TokenKind::Minus;
    if (negative) {
        index = 1;
    }
    // Exactly one literal, then the end of input.
    if (tokens->size() != index + 2) {
        return tl::unexpected(fmt::format("'{}' is not a single value", text));
    }
    return (*tokens)[index];
}

} // namespace

tl::expected<Value, std::string> parseValue(std::string_view text, DataType type) {
    if (type == DataType::Bool) {
        const auto upper = toUpper(text);
        if (upper == "TRUE" || upper == "1") return true;
        if (upper == "FALSE" || upper == "0") return false;
        return tl::unexpected(fmt::format("'{}' is not a BOOL (use TRUE or FALSE)", text));
    }

    bool negative = false;
    auto token = singleLiteral(text, negative);
    if (!token) {
        return tl::unexpected(token.error());
    }

    switch (type) {
    case DataType::Int:
    case DataType::DInt: {
        if (token->kind != TokenKind::Integer) {
            return tl::unexpected(fmt::format("'{}' is not an integer", text));
        }
        auto value = std::get<std::int64_t>(token->value);
        value = negative ? -value : value;
        const std::int64_t low = type == DataType::Int ? -32768 : -2147483648LL;
        const std::int64_t high = type == DataType::Int ? 32767 : 2147483647LL;
        if (value < low || value > high) {
            return tl::unexpected(fmt::format("{} doesn't fit in {} ({} to {})", value, dataTypeName(type), low, high));
        }
        return value;
    }
    case DataType::Real: {
        double value = 0.0;
        if (token->kind == TokenKind::Real) {
            value = std::get<double>(token->value);
        } else if (token->kind == TokenKind::Integer) {
            value = static_cast<double>(std::get<std::int64_t>(token->value));
        } else {
            return tl::unexpected(fmt::format("'{}' is not a number", text));
        }
        return negative ? -value : value;
    }
    case DataType::Time: {
        if (token->kind != TokenKind::Time) {
            return tl::unexpected(fmt::format("'{}' is not a TIME (write it like T#2s or T#500ms)", text));
        }
        const auto value = std::get<std::int64_t>(token->value);
        return negative ? -value : value;
    }
    default: return tl::unexpected(fmt::format("can't enter a value for a {}", dataTypeName(type)));
    }
}

} // namespace softplc::st
