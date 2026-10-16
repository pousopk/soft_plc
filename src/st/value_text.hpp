#pragma once
#include "st/ast.hpp"
#include <string>
#include <string_view>
#include <tl/expected.hpp>

namespace softplc::st {

// Reads a value typed by a person (in a monitoring tool) for a variable of
// the given type:
//   BOOL       TRUE, FALSE, 1, 0 (any case)
//   INT, DINT  -5, 16#FF, 1_000 — must fit the type
//   REAL       2.5, -1.0E3, 7
//   TIME       T#2s, T#1m30s
// This is the reverse of formatValue(value, type).
tl::expected<Value, std::string> parseValue(std::string_view text, DataType type);

} // namespace softplc::st
