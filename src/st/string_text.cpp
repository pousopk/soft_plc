#include "st/string_text.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <fmt/format.h>

namespace softplc::st {

namespace {

constexpr char kHexDigits[] = "0123456789ABCDEF";

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// One UTF-8 character starting at text[i]; moves i past it. Nothing if the
// bytes aren't valid UTF-8.
std::optional<char32_t> nextUtf8(std::string_view text, std::size_t& i) {
    const auto lead = static_cast<unsigned char>(text[i]);
    std::size_t count = 0;
    char32_t code = 0;
    char32_t smallest = 0;
    if (lead < 0x80) {
        ++i;
        return lead;
    }
    if (lead >= 0xC2 && lead <= 0xDF) {
        count = 1, code = lead & 0x1Fu, smallest = 0x80;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
        count = 2, code = lead & 0x0Fu, smallest = 0x800;
    } else if (lead >= 0xF0 && lead <= 0xF4) {
        count = 3, code = lead & 0x07u, smallest = 0x10000;
    } else {
        return std::nullopt;
    }
    if (i + count >= text.size()) {
        return std::nullopt; // fewer bytes left than the lead byte announces
    }
    for (std::size_t k = 1; k <= count; ++k) {
        const auto next = static_cast<unsigned char>(text[i + k]);
        if ((next & 0xC0u) != 0x80u) {
            return std::nullopt;
        }
        code = (code << 6) | (next & 0x3Fu);
    }
    if (code < smallest || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) {
        return std::nullopt;
    }
    i += count + 1;
    return code;
}

bool equalIgnoringCase(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::toupper(static_cast<unsigned char>(x)) == std::toupper(static_cast<unsigned char>(y));
           });
}

bool startsWithIgnoringCase(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && equalIgnoringCase(text.substr(0, prefix.size()), prefix);
}

// The ASCII text of a value, as formatValue writes it, into `out`; returns
// the length it needs (which can be more than out.size(): it's then cut).
std::size_t formatAscii(std::span<char> out, const Value& value, DataType type) {
    const auto write = [&]<typename... Args>(fmt::format_string<Args...> format, Args&&... args) {
        return fmt::format_to_n(out.data(), out.size(), format, std::forward<Args>(args)...).size;
    };
    if (const auto* b = std::get_if<bool>(&value)) {
        return write("{}", *b ? "TRUE" : "FALSE");
    }
    if (const auto* r = std::get_if<double>(&value)) {
        auto length = write("{}", *r);
        const std::string_view text(out.data(), std::min(length, out.size()));
        if (text.find_first_of(".eEn") == std::string_view::npos) { // like formatReal: always a decimal point
            for (const char c : {'.', '0'}) {
                if (length < out.size()) {
                    out[length] = c;
                }
                ++length;
            }
        }
        return length;
    }
    const auto number = std::get<std::int64_t>(value);
    if (isBitString(type)) {
        return write("16#{:0{}X}", static_cast<std::uint64_t>(number), bitWidth(type) / 4);
    }
    if (type != DataType::Time) {
        return write("{}", number);
    }
    // TIME, like formatTime: T#1d2h3m4s5ms, leaving out the zero parts.
    if (number == 0) {
        return write("T#0ms");
    }
    std::size_t length = write("{}", number < 0 ? "T#-" : "T#");
    auto rest = number < 0 ? -number : number;
    constexpr std::pair<std::int64_t, const char*> units[] = {
        {86'400'000, "d"}, {3'600'000, "h"}, {60'000, "m"}, {1'000, "s"}, {1, "ms"},
    };
    for (const auto& [size, unit] : units) {
        if (rest >= size) {
            const auto from = std::min(length, out.size());
            length += fmt::format_to_n(out.data() + from, out.size() - from, "{}{}", rest / size, unit).size;
            rest %= size;
        }
    }
    return length;
}

std::optional<Value> parseInteger(std::string_view text, DataType type) {
    const bool negative = !text.empty() && text[0] == '-';
    if (negative) {
        text.remove_prefix(1);
    }
    int base = 10;
    if (const auto hash = text.find('#'); hash != std::string_view::npos) {
        const auto prefix = text.substr(0, hash);
        base = prefix == "2" ? 2 : prefix == "8" ? 8 : prefix == "16" ? 16 : 0;
        if (base == 0) {
            return std::nullopt;
        }
        text.remove_prefix(hash + 1);
    }
    std::array<char, 80> digits{};
    std::size_t count = 0;
    for (const char c : text) {
        if (c != '_') {
            digits[count++] = c;
        }
    }
    std::uint64_t magnitude = 0;
    const auto [ptr, ec] = std::from_chars(digits.data(), digits.data() + count, magnitude, base);
    if (count == 0 || ec != std::errc{} || ptr != digits.data() + count) {
        return std::nullopt;
    }
    const auto [low, high] = integerRange(type);
    if (negative) {
        if (magnitude > static_cast<std::uint64_t>(-(low + 1)) + 1) {
            return std::nullopt;
        }
        return Value{static_cast<std::int64_t>(0 - magnitude)};
    }
    if (magnitude > static_cast<std::uint64_t>(high)) {
        return std::nullopt;
    }
    return Value{static_cast<std::int64_t>(magnitude)};
}

std::optional<Value> parseTime(std::string_view text) {
    if (startsWithIgnoringCase(text, "TIME#")) {
        text.remove_prefix(5);
    } else if (startsWithIgnoringCase(text, "T#")) {
        text.remove_prefix(2);
    } else {
        return std::nullopt;
    }
    const bool negative = !text.empty() && text[0] == '-';
    if (negative) {
        text.remove_prefix(1);
    }
    std::int64_t total = 0;
    bool anyPart = false;
    while (!text.empty()) {
        std::int64_t amount = 0;
        const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), amount);
        if (ec != std::errc{} || amount < 0) {
            return std::nullopt;
        }
        text.remove_prefix(static_cast<std::size_t>(ptr - text.data()));
        std::int64_t unit = 0;
        if (startsWithIgnoringCase(text, "ms")) {
            unit = 1, text.remove_prefix(2);
        } else if (!text.empty()) {
            const auto c = std::toupper(static_cast<unsigned char>(text[0]));
            unit = c == 'D' ? 86'400'000 : c == 'H' ? 3'600'000 : c == 'M' ? 60'000 : c == 'S' ? 1'000 : 0;
            text.remove_prefix(1);
        }
        if (unit == 0 || amount > std::numeric_limits<std::int32_t>::max() / unit) {
            return std::nullopt;
        }
        total += amount * unit;
        anyPart = true;
    }
    const auto [low, high] = integerRange(DataType::Time);
    const auto value = negative ? -total : total;
    if (!anyPart || value < low || value > high) {
        return std::nullopt;
    }
    return Value{value};
}

} // namespace

tl::expected<std::u16string, std::string> decodeLiteral(std::string_view inside, TextKind kind) {
    const bool wide = kind == TextKind::WString;
    std::u16string text;
    for (std::size_t i = 0; i < inside.size();) {
        if (inside[i] != '$') {
            const auto start = i;
            const auto code = nextUtf8(inside, i);
            if (!code) {
                return tl::unexpected(std::string("invalid UTF-8 in a string"));
            }
            if (*code > (wide ? 0xFFFFu : 0xFFu)) {
                return tl::unexpected(fmt::format("character '{}' doesn't fit in a {}: write it as {}",
                                                  inside.substr(start, i - start), wide ? "WSTRING" : "STRING",
                                                  wide ? "$hhhh" : "$hh"));
            }
            text += static_cast<char16_t>(*code);
            continue;
        }
        if (i + 1 >= inside.size()) {
            return tl::unexpected(std::string("a string can't end with a single '$' (write $$ for a dollar sign)"));
        }
        const char c = inside[i + 1];
        const std::size_t hexDigits = wide ? 4 : 2;
        if (hexValue(c) >= 0) {
            if (i + 1 + hexDigits > inside.size()) {
                return tl::unexpected(fmt::format("'$' needs {} hex digits here", hexDigits));
            }
            unsigned code = 0;
            for (std::size_t k = 0; k < hexDigits; ++k) {
                const int digit = hexValue(inside[i + 1 + k]);
                if (digit < 0) {
                    return tl::unexpected(fmt::format("'$' needs {} hex digits here", hexDigits));
                }
                code = code * 16 + static_cast<unsigned>(digit);
            }
            text += static_cast<char16_t>(code);
            i += 1 + hexDigits;
            continue;
        }
        switch (std::toupper(static_cast<unsigned char>(c))) {
        case '$': text += u'$'; break;
        case '\'': text += u'\''; break;
        case '"': text += u'"'; break;
        case 'L':
        case 'N': text += u'\n'; break;
        case 'P': text += u'\f'; break;
        case 'R': text += u'\r'; break;
        case 'T': text += u'\t'; break;
        default: return tl::unexpected(fmt::format("unknown escape '${}' in a string", c));
        }
        i += 2;
    }
    return text;
}

std::string encodeLiteral(std::u16string_view text, TextKind kind) {
    const bool wide = kind == TextKind::WString;
    const char quote = wide ? '"' : '\'';
    std::string out(1, quote);
    for (const char16_t c : text) {
        if (c == u'$') {
            out += "$$";
        } else if (c == static_cast<char16_t>(quote)) {
            out += '$';
            out += quote;
        } else if (c < 0x20 || c >= 0x7F) {
            out += '$';
            for (int shift = wide ? 12 : 4; shift >= 0; shift -= 4) {
                out += kHexDigits[(c >> shift) & 0xF];
            }
        } else {
            out += static_cast<char>(c);
        }
    }
    out += quote;
    return out;
}

int compareText(std::u16string_view a, std::u16string_view b) { return a.compare(b); }

std::size_t formatInto(std::span<char16_t> out, const Value& value, DataType type) {
    std::array<char, 64> ascii{};
    const auto length = std::min({formatAscii(ascii, value, type), ascii.size(), out.size()});
    std::copy_n(ascii.begin(), length, out.begin());
    return length;
}

std::optional<Value> parseText(std::u16string_view text, DataType type) {
    // Values are plain ASCII: narrow to a buffer on the stack.
    std::array<char, 80> ascii{};
    if (text.size() > ascii.size()) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] > 0x7F) {
            return std::nullopt;
        }
        ascii[i] = static_cast<char>(text[i]);
    }
    std::string_view s(ascii.data(), text.size());
    while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
    while (!s.empty() && s.back() == ' ') s.remove_suffix(1);

    // A typed literal of this very type: WORD#16#FF for a WORD.
    const std::string_view name = dataTypeName(type);
    if (s.size() > name.size() && s[name.size()] == '#' && startsWithIgnoringCase(s, name) && type != DataType::Time) {
        s.remove_prefix(name.size() + 1);
    }

    if (type == DataType::Bool) {
        if (equalIgnoringCase(s, "TRUE") || s == "1") return Value{true};
        if (equalIgnoringCase(s, "FALSE") || s == "0") return Value{false};
        return std::nullopt;
    }
    if (type == DataType::Time) {
        return parseTime(s);
    }
    if (type == DataType::Real) {
        double value = 0.0;
        const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
        if (s.empty() || ec != std::errc{} || ptr != s.data() + s.size()) {
            return std::nullopt;
        }
        return Value{value};
    }
    if (isIntegerType(type) || isBitString(type)) {
        return parseInteger(s, type);
    }
    return std::nullopt;
}

} // namespace softplc::st
