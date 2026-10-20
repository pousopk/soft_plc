#pragma once
#include "st/ast.hpp"
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tl/expected.hpp>

// Text and strings: literals as written in programs and in the monitor,
// comparing, and turning values into text and back (for INT_TO_STRING,
// STRING_TO_INT and the like).
//
// formatInto and parseText run inside the scan cycle, so they never
// allocate: they work in fixed buffers on the stack.
namespace softplc::st {

// 'single quotes' for STRING and CHAR (1-byte characters), "double quotes"
// for WSTRING and WCHAR (2-byte characters).
enum class TextKind { String, WString };

// The text between a literal's quotes, with $ escapes resolved ($$, $', $",
// $L, $N, $P, $R, $T, $hh, or $hhhh in a WSTRING) and UTF-8 decoded. A
// STRING character must be 0..255.
tl::expected<std::u16string, std::string> decodeLiteral(std::string_view inside, TextKind kind);

// `text` as a literal, quotes included, in plain ASCII: $, the quote and
// control characters are escaped, and so are bytes from 16#80 up ($E9) or,
// in a WSTRING, characters from 16#80 up ($20AC).
std::string encodeLiteral(std::u16string_view text, TextKind kind);

// Negative, zero or positive, comparing character codes from the left; a
// text that's the start of another is smaller.
int compareText(std::u16string_view a, std::u16string_view b);

// The text of `value` (of an elementary type) as formatValue writes it,
// without allocating. Writes at most out.size() characters, returns how many.
std::size_t formatInto(std::span<char16_t> out, const Value& value, DataType type);

// The value of a text, in the forms parseValue accepts (42, 16#FF,
// WORD#16#FF, 2.5, T#1s, TRUE), without allocating. Nothing if it isn't a
// value of the type, or doesn't fit it.
std::optional<Value> parseText(std::u16string_view text, DataType type);

} // namespace softplc::st
