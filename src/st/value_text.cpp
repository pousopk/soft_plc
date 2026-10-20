#include "st/value_text.hpp"
#include "st/lexer.hpp"
#include "util/text.hpp"
#include <charconv>
#include <fmt/format.h>

namespace softplc::st {

namespace {

// Reuses the Structured Text lexer, so values are written exactly like in a
// program: an optional type (WORD#), an optional minus, then one literal.
tl::expected<Token, std::string> singleLiteral(std::string_view text, bool& negative, DataType& typed) {
    auto tokens = tokenize(text);
    if (!tokens) {
        return tl::unexpected(tokens.error().message);
    }
    std::size_t index = 0;
    typed = DataType::Unknown;
    if ((*tokens)[index].kind == TokenKind::TypePrefix) {
        typed = elementaryTypeFromName((*tokens)[index].text).value_or(DataType::Unknown);
        ++index;
    }
    negative = (*tokens)[index].kind == TokenKind::Minus;
    if (negative) {
        ++index;
    }
    // Exactly one literal, then the end of input.
    if (tokens->size() != index + 2) {
        return tl::unexpected(fmt::format("'{}' is not a single value", text));
    }
    return (*tokens)[index];
}

// "a WORD", "an INT".
std::string withArticle(DataType type) {
    const std::string name = dataTypeName(type);
    return (name[0] == 'I' ? "an " : "a ") + name;
}

} // namespace

tl::expected<Value, std::string> parseValue(std::string_view text, DataType type) {
    if (type == DataType::Bool) {
        const auto upper = toUpper(text);
        if (upper == "TRUE" || upper == "1") return true;
        if (upper == "FALSE" || upper == "0") return false;
        return tl::unexpected(fmt::format("'{}' is not a BOOL (use TRUE or FALSE)", text));
    }

    if (isIntegerType(type) || isBitString(type)) {
        // Plain decimals first, so that all of LINT works: the lexer reads
        // -9223372036854775808 as a minus and a number too big on its own.
        std::int64_t value = 0;
        const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (ec != std::errc{} || ptr != text.data() + text.size()) {
            bool negative = false;
            auto typed = DataType::Unknown;
            auto token = singleLiteral(text, negative, typed);
            if (!token) {
                return tl::unexpected(token.error());
            }
            if (typed != DataType::Unknown && typed != type) {
                return tl::unexpected(fmt::format("'{}' is {}, not {}", text, withArticle(typed), withArticle(type)));
            }
            if (token->kind != TokenKind::Integer) {
                return tl::unexpected(fmt::format("'{}' is not an integer", text));
            }
            value = std::get<std::int64_t>(token->value);
            value = negative ? -value : value;
        }
        const auto [low, high] = integerRange(type);
        if (value < low || value > high) {
            return tl::unexpected(fmt::format("{} doesn't fit in {} ({} to {})", value, dataTypeName(type), low, high));
        }
        return value;
    }

    bool negative = false;
    auto typed = DataType::Unknown;
    auto token = singleLiteral(text, negative, typed);
    if (!token) {
        return tl::unexpected(token.error());
    }
    if (typed != DataType::Unknown && typed != type) {
        return tl::unexpected(fmt::format("'{}' is {}, not {}", text, withArticle(typed), withArticle(type)));
    }

    switch (type) {
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
