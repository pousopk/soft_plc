#include "st/functions.hpp"
#include "st/interpreter.hpp"
#include "st/runtime_values.hpp"
#include "util/text.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <fmt/format.h>
#include <limits>

namespace softplc::st {

namespace {

using namespace runtime;

struct NamedFunction {
    std::string_view name;
    BuiltinFunction function;
};

constexpr NamedFunction kFunctions[] = {
    {"ABS", BuiltinFunction::Abs},     {"SQRT", BuiltinFunction::Sqrt},   {"LN", BuiltinFunction::Ln},
    {"LOG", BuiltinFunction::Log},     {"EXP", BuiltinFunction::Exp},     {"SIN", BuiltinFunction::Sin},
    {"COS", BuiltinFunction::Cos},     {"TAN", BuiltinFunction::Tan},     {"ASIN", BuiltinFunction::Asin},
    {"ACOS", BuiltinFunction::Acos},   {"ATAN", BuiltinFunction::Atan},   {"EXPT", BuiltinFunction::Expt},
    {"TRUNC", BuiltinFunction::Trunc}, {"ROUND", BuiltinFunction::Round}, {"CEIL", BuiltinFunction::Ceil},
    {"FLOOR", BuiltinFunction::Floor}, {"MIN", BuiltinFunction::Min},     {"MAX", BuiltinFunction::Max},
    {"LIMIT", BuiltinFunction::Limit}, {"SEL", BuiltinFunction::Sel},     {"MUX", BuiltinFunction::Mux},
    {"SHL", BuiltinFunction::Shl},     {"SHR", BuiltinFunction::Shr},     {"ROL", BuiltinFunction::Rol},
    {"ROR", BuiltinFunction::Ror},     {"LEN", BuiltinFunction::Len},     {"LEFT", BuiltinFunction::Left},
    {"RIGHT", BuiltinFunction::Right}, {"MID", BuiltinFunction::Mid},     {"CONCAT", BuiltinFunction::Concat},
    {"INSERT", BuiltinFunction::Insert}, {"DELETE", BuiltinFunction::Delete}, {"REPLACE", BuiltinFunction::Replace},
    {"FIND", BuiltinFunction::Find},
};

// Whether FROM_TO_TO exists: between any integers and bit strings, integers
// and REAL, BOOL and integers or bit strings; TIME only to and from DINT
// (its milliseconds); and DWORD and REAL, which copies the bits.
bool conversionExists(DataType from, DataType to) {
    if (from == to) {
        return false;
    }
    const auto bitsOrInteger = [](DataType t) { return isIntegerType(t) || isBitString(t); };
    if (from == DataType::Time || to == DataType::Time) {
        return from == DataType::DInt || to == DataType::DInt;
    }
    if (from == DataType::Real || to == DataType::Real) {
        const auto other = from == DataType::Real ? to : from;
        return isIntegerType(other) || other == DataType::DWord;
    }
    if (from == DataType::Bool || to == DataType::Bool) {
        return bitsOrInteger(from == DataType::Bool ? to : from);
    }
    return bitsOrInteger(from) && bitsOrInteger(to);
}

// A REAL turned into an integer type: the value is clamped to the type's
// range (there are no bits to wrap), and NaN gives 0. The comparisons are
// done on doubles first: casting a double too big for an int64 is undefined.
std::int64_t realToInteger(double value, DataType type) {
    if (std::isnan(value)) {
        return 0;
    }
    const auto [low, high] = integerRange(type);
    if (value <= static_cast<double>(low)) {
        return low;
    }
    if (value >= static_cast<double>(high)) {
        return high;
    }
    return static_cast<std::int64_t>(value);
}

// SHL, SHR, ROL and ROR on a bit string of `type`: shifting by its width or
// more gives 0, rotating goes round (N MOD width), and a negative N changes nothing.
std::int64_t shiftBits(BuiltinFunction function, std::int64_t value, std::int64_t n, DataType type) {
    const auto width = bitWidth(type);
    const auto in = static_cast<std::uint64_t>(value);
    if (n <= 0) {
        return value;
    }
    std::uint64_t bits = 0;
    if (function == BuiltinFunction::Shl || function == BuiltinFunction::Shr) {
        if (n < width) {
            bits = function == BuiltinFunction::Shl ? in << n : in >> n;
        }
    } else {
        const auto s = static_cast<unsigned>(n % width);
        bits = s == 0 ? in
               : function == BuiltinFunction::Rol ? (in << s) | (in >> (width - s))
                                                  : (in >> s) | (in << (width - s));
    }
    return wrapInteger(static_cast<std::int64_t>(bits), type);
}

// Round to the nearest whole number, halves to the even one (IEEE 754's
// default rounding): 2.5 -> 2, 3.5 -> 4, -2.5 -> -2.
double roundHalfEven(double value) {
    const double down = std::floor(value);
    const double fraction = value - down;
    if (fraction > 0.5)
        return down + 1.0;
    if (fraction < 0.5)
        return down;
    return std::fmod(down, 2.0) == 0.0 ? down : down + 1.0;
}

bool isReal(const Value& value) {
    return std::holds_alternative<double>(value);
}

// < for two values of the same kind (both integers, both REALs).
bool less(const Value& a, const Value& b) {
    if (isReal(a) || isReal(b)) {
        return toReal(a) < toReal(b);
    }
    return toInteger(a) < toInteger(b);
}

// A value as the result type: integers wrapped to their size, REALs rounded to 32 bits.
Value asResult(const Value& value, DataType result) {
    if (result == DataType::Bool) {
        return std::get<bool>(value);
    }
    if (result == DataType::Real) {
        return roundToReal(toReal(value));
    }
    return wrapInteger(toInteger(value), result);
}

} // namespace

std::optional<BuiltinSignature> findBuiltinFunction(std::string_view name) {
    const auto upper = toUpper(name);
    for (const auto& entry : kFunctions) {
        if (upper == entry.name) {
            return BuiltinSignature{entry.function};
        }
    }
    for (const auto from : elementaryTypes()) {
        for (const auto to : elementaryTypes()) {
            if (conversionExists(from, to) && upper == fmt::format("{}_TO_{}", dataTypeName(from), dataTypeName(to))) {
                const auto function = from == DataType::DWord && to == DataType::Real   ? BuiltinFunction::DWordToReal
                                      : from == DataType::Real && to == DataType::DWord ? BuiltinFunction::RealToDWord
                                                                                        : BuiltinFunction::Convert;
                return BuiltinSignature{function, from, to};
            }
        }
    }
    return std::nullopt;
}

std::vector<std::string> builtinInputNames(BuiltinFunction function, std::size_t count) {
    switch (function) {
    case BuiltinFunction::Expt:
        return {"IN1", "IN2"};
    case BuiltinFunction::Limit:
        return {"MN", "IN", "MX"};
    case BuiltinFunction::Sel:
        return {"G", "IN0", "IN1"};
    case BuiltinFunction::Shl:
    case BuiltinFunction::Shr:
    case BuiltinFunction::Rol:
    case BuiltinFunction::Ror:
        return {"IN", "N"};
    case BuiltinFunction::Left:
    case BuiltinFunction::Right:
        return {"IN", "L"};
    case BuiltinFunction::Mid:
    case BuiltinFunction::Delete:
        return {"IN", "L", "P"};
    case BuiltinFunction::Insert:
        return {"IN1", "IN2", "P"};
    case BuiltinFunction::Replace:
        return {"IN1", "IN2", "L", "P"};
    case BuiltinFunction::Find:
        return {"IN1", "IN2"};
    case BuiltinFunction::Concat: {
        std::vector<std::string> names;
        for (std::size_t i = 1; i <= count; ++i) {
            names.push_back(fmt::format("IN{}", i));
        }
        return names;
    }
    case BuiltinFunction::Min:
    case BuiltinFunction::Max: {
        std::vector<std::string> names;
        for (std::size_t i = 1; i <= count; ++i) {
            names.push_back(fmt::format("IN{}", i));
        }
        return names;
    }
    case BuiltinFunction::Mux: {
        std::vector<std::string> names{"K"};
        for (std::size_t i = 0; i + 1 < count; ++i) {
            names.push_back(fmt::format("IN{}", i));
        }
        return names;
    }
    default:
        return {"IN"};
    }
}

Value applyBuiltin(BuiltinFunction function, DataType result, std::span<const Value> arguments, SourceLocation where) {
    const auto real = [&](std::size_t i) { return toReal(arguments[i]); };
    switch (function) {
    case BuiltinFunction::Abs: {
        if (isReal(arguments[0])) {
            return std::fabs(real(0));
        }
        const auto value = toInteger(arguments[0]);
        return wrapInteger(value < 0 ? subtract(0, value) : value, result); // ABS of -32768 (INT) wraps, as on a PLC
    }
    case BuiltinFunction::Sqrt:
        return roundToReal(std::sqrt(real(0)));
    case BuiltinFunction::Ln:
        return roundToReal(std::log(real(0)));
    case BuiltinFunction::Log:
        return roundToReal(std::log10(real(0)));
    case BuiltinFunction::Exp:
        return roundToReal(std::exp(real(0)));
    case BuiltinFunction::Sin:
        return roundToReal(std::sin(real(0)));
    case BuiltinFunction::Cos:
        return roundToReal(std::cos(real(0)));
    case BuiltinFunction::Tan:
        return roundToReal(std::tan(real(0)));
    case BuiltinFunction::Asin:
        return roundToReal(std::asin(real(0)));
    case BuiltinFunction::Acos:
        return roundToReal(std::acos(real(0)));
    case BuiltinFunction::Atan:
        return roundToReal(std::atan(real(0)));
    case BuiltinFunction::Expt:
        return roundToReal(std::pow(real(0), real(1)));
    case BuiltinFunction::Trunc:
        return realToInteger(std::trunc(real(0)), DataType::DInt);
    case BuiltinFunction::Round:
        return realToInteger(roundHalfEven(real(0)), DataType::DInt);
    case BuiltinFunction::Ceil:
        return realToInteger(std::ceil(real(0)), DataType::DInt);
    case BuiltinFunction::Floor:
        return realToInteger(std::floor(real(0)), DataType::DInt);

    case BuiltinFunction::Min:
    case BuiltinFunction::Max: {
        Value best = arguments[0];
        for (std::size_t i = 1; i < arguments.size(); ++i) {
            const bool better = function == BuiltinFunction::Min ? less(arguments[i], best) : less(best, arguments[i]);
            if (better) {
                best = arguments[i];
            }
        }
        return asResult(best, result);
    }
    case BuiltinFunction::Limit: {
        // MIN(MAX(IN, MN), MX), as IEC defines it.
        const Value& low = arguments[0];
        const Value& high = arguments[2];
        const Value& in = arguments[1];
        const Value& raised = less(in, low) ? low : in;
        return asResult(less(high, raised) ? high : raised, result);
    }
    case BuiltinFunction::Sel:
        return asResult(std::get<bool>(arguments[0]) ? arguments[2] : arguments[1], result);
    case BuiltinFunction::Mux: {
        const auto k = toInteger(arguments[0]);
        const auto inputs = static_cast<std::int64_t>(arguments.size()) - 1;
        if (k < 0 || k >= inputs) {
            throw RuntimeError{fmt::format("MUX selector K = {} is outside 0..{}", k, inputs - 1), where};
        }
        return asResult(arguments[static_cast<std::size_t>(k) + 1], result);
    }

    case BuiltinFunction::Convert: {
        const Value& in = arguments[0];
        switch (result) {
        case DataType::Bool:
            return toInteger(in) != 0;
        case DataType::Real:
            return roundToReal(toReal(in));
        default:
            if (const auto* b = std::get_if<bool>(&in)) {
                return std::int64_t{*b ? 1 : 0};
            }
            if (isReal(in)) {
                return realToInteger(roundHalfEven(toReal(in)), result); // REAL_TO_INT, REAL_TO_DINT
            }
            return wrapInteger(toInteger(in), result); // DINT_TO_INT keeps the low 16 bits, like a PLC
        }
    }
    case BuiltinFunction::DWordToReal:
        return static_cast<double>(std::bit_cast<float>(static_cast<std::uint32_t>(toInteger(arguments[0]))));
    case BuiltinFunction::RealToDWord:
        return std::int64_t{std::bit_cast<std::uint32_t>(static_cast<float>(toReal(arguments[0])))};
    case BuiltinFunction::Shl:
    case BuiltinFunction::Shr:
    case BuiltinFunction::Rol:
    case BuiltinFunction::Ror: return shiftBits(function, toInteger(arguments[0]), toInteger(arguments[1]), result);
    case BuiltinFunction::Len: // texts: applyStringBuiltin computes these
    case BuiltinFunction::Left:
    case BuiltinFunction::Right:
    case BuiltinFunction::Mid:
    case BuiltinFunction::Concat:
    case BuiltinFunction::Insert:
    case BuiltinFunction::Delete:
    case BuiltinFunction::Replace:
    case BuiltinFunction::Find:
    case BuiltinFunction::None:
        break;
    }
    throw RuntimeError{"internal error: unknown standard function", where};
}

} // namespace softplc::st
