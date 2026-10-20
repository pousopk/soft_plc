#pragma once
#include "st/ast.hpp"
#include "st/diagnostic.hpp"
#include "st/string_pool.hpp"
#include <cstdint>
#include <span>

// The standard functions on texts: LEN, LEFT, RIGHT, MID, CONCAT, INSERT,
// DELETE, REPLACE and FIND, as IEC 61131-3 defines them (positions count
// from 1). Shared by both engines, like applyBuiltin.
namespace softplc::st {

// Whether `function` works on texts (computed by applyStringBuiltin, not applyBuiltin).
bool isStringFunction(BuiltinFunction function);

// Computes a text function. Texts arrive as handles into `pool`, positions
// and lengths as integers. A text result is made in `resultTemp` (cut to
// its capacity) and its handle returned; LEN and FIND give an integer.
// Throws RuntimeError (at `where`) for a position or length outside the text.
Value applyStringBuiltin(StringPool& pool, BuiltinFunction function, std::span<const Value> arguments,
                         std::int64_t resultTemp, SourceLocation where);

} // namespace softplc::st
