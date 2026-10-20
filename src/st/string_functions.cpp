#include "st/string_functions.hpp"
#include "st/interpreter.hpp"
#include "st/runtime_values.hpp"
#include "st/string_text.hpp"
#include <array>
#include <fmt/format.h>
#include <string_view>

namespace softplc::st {

namespace {

using namespace runtime;

// Throws unless `value` (called `name`, an input of `function`) is within low..high.
void checkRange(const char* function, const char* name, std::int64_t value, std::int64_t low, std::size_t high,
                SourceLocation where) {
    if (value < low || value > static_cast<std::int64_t>(high)) {
        throw RuntimeError{fmt::format("{}: {} = {} is outside {}..{}", function, name, value, low, high), where};
    }
}

// Throws unless L characters from position P are all inside a text of `length`.
void checkSpan(const char* function, std::int64_t l, std::int64_t p, std::size_t length, SourceLocation where) {
    checkRange(function, "P", p, 1, length, where);
    if (l < 0 || p - 1 + l > static_cast<std::int64_t>(length)) {
        throw RuntimeError{
            fmt::format("{}: L = {} from P = {} goes past the end of a text of {}", function, l, p, length), where};
    }
}

// Sets `handle`'s text to the pieces one after the other, cut to its capacity.
void join(StringPool& pool, std::int64_t handle, std::initializer_list<std::u16string_view> pieces) {
    std::size_t length = 0;
    for (const auto piece : pieces) {
        for (const char16_t c : piece) {
            if (length == pool.capacity(handle)) {
                return;
            }
            pool.setLength(handle, length + 1);
            pool.setChar(handle, length++, c);
        }
    }
    pool.setLength(handle, length);
}

std::size_t at(std::int64_t position) { return static_cast<std::size_t>(position); }

} // namespace

bool isStringFunction(BuiltinFunction function) {
    switch (function) {
    case BuiltinFunction::Len:
    case BuiltinFunction::Left:
    case BuiltinFunction::Right:
    case BuiltinFunction::Mid:
    case BuiltinFunction::Concat:
    case BuiltinFunction::Insert:
    case BuiltinFunction::Delete:
    case BuiltinFunction::Replace:
    case BuiltinFunction::Find:
    case BuiltinFunction::ToString:
    case BuiltinFunction::FromString:
    case BuiltinFunction::TextToText:
    case BuiltinFunction::CharToText:
    case BuiltinFunction::TextToChar: return true;
    default: return false;
    }
}

Value applyStringBuiltin(StringPool& pool, BuiltinFunction function, std::span<const Value> arguments,
                         std::int64_t resultTemp, DataType from, DataType to, SourceLocation where) {
    const auto text = [&](std::size_t i) { return pool.view(toInteger(arguments[i])); };
    const auto number = [&](std::size_t i) { return toInteger(arguments[i]); };
    switch (function) {
    case BuiltinFunction::Len: return static_cast<std::int64_t>(text(0).size());
    case BuiltinFunction::Left: {
        const auto in = text(0);
        checkRange("LEFT", "L", number(1), 0, in.size(), where);
        join(pool, resultTemp, {in.substr(0, at(number(1)))});
        return resultTemp;
    }
    case BuiltinFunction::Right: {
        const auto in = text(0);
        checkRange("RIGHT", "L", number(1), 0, in.size(), where);
        join(pool, resultTemp, {in.substr(in.size() - at(number(1)))});
        return resultTemp;
    }
    case BuiltinFunction::Mid: {
        const auto in = text(0);
        checkSpan("MID", number(1), number(2), in.size(), where);
        join(pool, resultTemp, {in.substr(at(number(2)) - 1, at(number(1)))});
        return resultTemp;
    }
    case BuiltinFunction::Concat: {
        // Any number of inputs: appended one by one, after the first.
        join(pool, resultTemp, {text(0)});
        for (std::size_t i = 1; i < arguments.size(); ++i) {
            const auto done = pool.view(resultTemp);
            join(pool, resultTemp, {done, text(i)});
        }
        return resultTemp;
    }
    case BuiltinFunction::Insert: { // IN2 into IN1, after position P (0: at the start)
        const auto in1 = text(0);
        checkRange("INSERT", "P", number(2), 0, in1.size(), where);
        const auto p = at(number(2));
        join(pool, resultTemp, {in1.substr(0, p), text(1), in1.substr(p)});
        return resultTemp;
    }
    case BuiltinFunction::Delete: { // L characters from position P
        const auto in = text(0);
        checkSpan("DELETE", number(1), number(2), in.size(), where);
        const auto p = at(number(2)) - 1;
        join(pool, resultTemp, {in.substr(0, p), in.substr(p + at(number(1)))});
        return resultTemp;
    }
    case BuiltinFunction::Replace: { // L characters from position P of IN1, replaced by IN2
        const auto in1 = text(0);
        checkSpan("REPLACE", number(2), number(3), in1.size(), where);
        const auto p = at(number(3)) - 1;
        join(pool, resultTemp, {in1.substr(0, p), text(1), in1.substr(p + at(number(2)))});
        return resultTemp;
    }
    case BuiltinFunction::Find: { // where IN2 first appears in IN1, from 1; 0 if nowhere
        const auto in2 = text(1);
        const auto found = in2.empty() ? std::u16string_view::npos : text(0).find(in2);
        return found == std::u16string_view::npos ? std::int64_t{0} : static_cast<std::int64_t>(found + 1);
    }
    case BuiltinFunction::ToString: { // the text the monitor shows, made on the stack
        std::array<char16_t, 64> buffer{};
        const auto length = formatInto(buffer, arguments[0], from);
        join(pool, resultTemp, {std::u16string_view(buffer.data(), length)});
        return resultTemp;
    }
    case BuiltinFunction::FromString: {
        if (auto value = parseText(text(0), to)) {
            return *value;
        }
        const auto name = std::string_view(dataTypeName(to));
        throw RuntimeError{fmt::format("{}_TO_{}: {} is not {} {}", dataTypeName(from), name,
                                       encodeLiteral(text(0), from == DataType::String ? TextKind::String : TextKind::WString),
                                       name[0] == 'I' ? "an" : "a", name), // an INT, a USINT
                           where};
    }
    case BuiltinFunction::TextToText: { // to a STRING, a character beyond 255 becomes '?'
        const auto in = text(0);
        join(pool, resultTemp, {in});
        if (to == DataType::String) {
            for (std::size_t k = 0; k < pool.view(resultTemp).size(); ++k) {
                if (in[k] > 0xFF) {
                    pool.setChar(resultTemp, k, u'?');
                }
            }
        }
        return resultTemp;
    }
    case BuiltinFunction::CharToText: {
        const auto c = static_cast<char16_t>(number(0));
        join(pool, resultTemp, {std::u16string_view(&c, 1)});
        return resultTemp;
    }
    case BuiltinFunction::TextToChar: { // the first character, or 0 for an empty text
        const auto in = text(0);
        return in.empty() ? std::int64_t{0} : std::int64_t{in[0]};
    }
    default: break;
    }
    throw RuntimeError{"internal error: not a text function", where};
}

} // namespace softplc::st
