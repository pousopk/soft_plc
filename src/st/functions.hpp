#pragma once
#include "st/ast.hpp"
#include "st/diagnostic.hpp"
#include <optional>
#include <span>
#include <string_view>

// The standard functions: ABS, SQRT, MIN, MAX, LIMIT, SEL, MUX, TRUNC,
// ROUND, the conversions like DINT_TO_INT, ...
//
// They are built in rather than written in ST because most of them work on
// several types (MAX of INTs gives an INT, MAX of REALs a REAL), which a
// FUNCTION written in ST can't do. The analyzer checks the arguments and
// works out the types; applyBuiltin() computes the result, for both engines.
namespace softplc::st {

// What a standard function's name means.
struct BuiltinSignature {
    BuiltinFunction function = BuiltinFunction::None;
    DataType from = DataType::Unknown; // Convert: the input type
    DataType to = DataType::Unknown;   // Convert: the result type
};

// The standard function with this name (any case), if there is one.
std::optional<BuiltinSignature> findBuiltinFunction(std::string_view name);

// The names of the standard function's inputs, in order, for calls that
// give arguments by name. `count` is how many arguments the call has (MIN,
// MAX and MUX take any number).
std::vector<std::string> builtinInputNames(BuiltinFunction function, std::size_t count);

// Computes a standard function. The arguments are already converted to the
// types the analyzer chose; `result` is the type of the result. Throws
// RuntimeError (at `where`) for MUX with a selector out of range.
Value applyBuiltin(BuiltinFunction function, DataType result, std::span<const Value> arguments, SourceLocation where);

// The most arguments a standard function call or a FUNCTION call can have.
constexpr std::size_t kMaxCallArguments = 32;

} // namespace softplc::st
