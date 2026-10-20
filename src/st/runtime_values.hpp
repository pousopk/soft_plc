#pragma once
#include "st/ast.hpp"
#include <cstdint>
#include <variant>

// How values behave at run time: wrapping, rounding and converting. Shared by
// the tree-walking interpreter and the bytecode VM, so both engines give
// exactly the same results.
namespace softplc::st::runtime {

// A BOOL counts as 0 or 1, for when it's widened to a bit string (flag OR myWord).
inline std::int64_t toInteger(const Value& value) {
    if (const auto* b = std::get_if<bool>(&value)) {
        return *b ? 1 : 0;
    }
    return std::get<std::int64_t>(value);
}

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
    case DataType::SInt: return static_cast<std::int8_t>(value);
    case DataType::USInt:
    case DataType::Byte:
    case DataType::Char: return static_cast<std::uint8_t>(value);
    case DataType::Int: return static_cast<std::int16_t>(value);
    case DataType::UInt:
    case DataType::Word:
    case DataType::WChar: return static_cast<std::uint16_t>(value);
    case DataType::DInt:
    case DataType::Time: return static_cast<std::int32_t>(value);
    case DataType::UDInt:
    case DataType::DWord: return static_cast<std::uint32_t>(value);
    default: return value; // LINT, and AnyInt: a plain 64-bit calculation of literals
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

// A part of an integer or bit string, like myWord.%X3: the bits, as a BOOL
// for a single bit.
inline Value extractPart(std::int64_t whole, const BitPart& part) {
    const auto mask = (std::uint64_t{1} << part.width) - 1;
    const auto bits = (static_cast<std::uint64_t>(whole) >> part.shift) & mask;
    if (part.width == 1) {
        return bits != 0;
    }
    return static_cast<std::int64_t>(bits);
}

// `whole` with a part replaced by `value`, the rest unchanged.
inline std::int64_t insertPart(std::int64_t whole, const BitPart& part, const Value& value) {
    const auto mask = ((std::uint64_t{1} << part.width) - 1) << part.shift;
    const auto bits = (static_cast<std::uint64_t>(whole) & ~mask) |
                      ((static_cast<std::uint64_t>(toInteger(value)) << part.shift) & mask);
    return wrapInteger(static_cast<std::int64_t>(bits), part.whole);
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
