#pragma once
#include "st/ast.hpp"
#include <cstdint>
#include <variant>

// How values behave at run time: wrapping, rounding and converting. Shared by
// the tree-walking interpreter and the bytecode VM, so both engines give
// exactly the same results.
namespace softplc::st::runtime {

inline std::int64_t toInteger(const Value& value) { return std::get<std::int64_t>(value); }

inline double toReal(const Value& value) {
    if (const auto* i = std::get_if<std::int64_t>(&value)) {
        return static_cast<double>(*i);
    }
    return std::get<double>(value);
}

// REAL is a 32-bit float: values are held in a double, but rounded to float
// precision after every operation so results match a real PLC.
inline double roundToReal(double value) { return static_cast<double>(static_cast<float>(value)); }

// Brings an integer into its type's range by wrapping around, exactly like a
// PLC does: INT 32767 + 1 gives -32768. Since C++20, converting to a smaller
// signed type is guaranteed to wrap this way.
inline std::int64_t wrapInteger(std::int64_t value, DataType type) {
    switch (type) {
    case DataType::Int: return static_cast<std::int16_t>(value);
    case DataType::DInt:
    case DataType::Time: return static_cast<std::int32_t>(value);
    default: return value; // AnyInt: a plain 64-bit calculation of literals
    }
}

// Converts a value to the given type, for storing in a variable.
inline Value convert(const Value& value, DataType type) {
    switch (type) {
    case DataType::Bool: return std::get<bool>(value);
    case DataType::Real: return roundToReal(toReal(value));
    default: return wrapInteger(toInteger(value), type);
    }
}

inline Value defaultValue(DataType type) {
    switch (type) {
    case DataType::Bool: return false;
    case DataType::Real: return 0.0;
    default: return std::int64_t{0};
    }
}

// 64-bit integer arithmetic without undefined behaviour. Signed overflow is
// undefined behaviour in C++, so + - * are done on unsigned numbers (which
// wrap) and converted back.

inline std::int64_t add(std::int64_t a, std::int64_t b) {
    return static_cast<std::int64_t>(static_cast<std::uint64_t>(a) + static_cast<std::uint64_t>(b));
}
inline std::int64_t subtract(std::int64_t a, std::int64_t b) {
    return static_cast<std::int64_t>(static_cast<std::uint64_t>(a) - static_cast<std::uint64_t>(b));
}
inline std::int64_t multiply(std::int64_t a, std::int64_t b) {
    return static_cast<std::int64_t>(static_cast<std::uint64_t>(a) * static_cast<std::uint64_t>(b));
}

// Integer division and modulo, rounding towards zero like IEC (-7 / 2 = -3,
// -7 MOD 2 = -1). The caller checks for division by zero; b == -1 is handled
// here to avoid the one overflowing case, INT64_MIN / -1.
inline std::int64_t divide(std::int64_t a, std::int64_t b) { return b == -1 ? subtract(0, a) : a / b; }
inline std::int64_t modulo(std::int64_t a, std::int64_t b) { return b == -1 ? 0 : a % b; }

} // namespace softplc::st::runtime
