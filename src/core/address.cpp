#include "core/address.hpp"
#include <charconv>
#include <fmt/format.h>

namespace softplc {

namespace {

tl::unexpected<std::string> fail(std::string_view text, std::string_view reason) {
    return tl::unexpected(fmt::format("invalid address '{}': {}", text, reason));
}

// Reads a run of decimal digits from the front of `rest` and advances past it.
bool consumeNumber(std::string_view& rest, std::size_t& value) {
    const auto* begin = rest.data();
    const auto* end = rest.data() + rest.size();
    const auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc{} || ptr == begin) {
        return false;
    }
    rest.remove_prefix(static_cast<std::size_t>(ptr - begin));
    return true;
}

} // namespace

tl::expected<Address, std::string> parseAddress(std::string_view text) {
    std::string_view rest = text;
    if (!rest.empty() && rest.front() == '%') {
        rest.remove_prefix(1);
    }

    Address address;
    if (rest.empty()) {
        return fail(text, "missing area (I, Q or M)");
    }
    switch (rest.front()) {
    case 'I': address.area = Area::Input; break;
    case 'Q': address.area = Area::Output; break;
    case 'M': address.area = Area::Marker; break;
    default: return fail(text, "area must be I, Q or M");
    }
    rest.remove_prefix(1);

    // The size letter is optional; without one, the address is a bit address.
    bool explicitSize = true;
    if (!rest.empty()) {
        switch (rest.front()) {
        case 'X': address.size = AccessSize::Bit; break;
        case 'B': address.size = AccessSize::Byte; break;
        case 'W': address.size = AccessSize::Word; break;
        case 'D': address.size = AccessSize::DWord; break;
        default: explicitSize = false; break;
        }
        if (explicitSize) {
            rest.remove_prefix(1);
        }
    }

    if (!consumeNumber(rest, address.byteOffset)) {
        return fail(text, "missing byte number");
    }

    if (address.size == AccessSize::Bit) {
        if (rest.empty() || rest.front() != '.') {
            return fail(text, "bit address needs '.<bit>', e.g. %I0.3");
        }
        rest.remove_prefix(1);
        std::size_t bit = 0;
        if (!consumeNumber(rest, bit) || bit > 7) {
            return fail(text, "bit number must be 0-7");
        }
        address.bit = static_cast<unsigned>(bit);
    }

    if (!rest.empty()) {
        return fail(text, fmt::format("unexpected '{}'", rest));
    }
    return address;
}

std::string Address::toString() const {
    char areaLetter = 'I';
    switch (area) {
    case Area::Input: areaLetter = 'I'; break;
    case Area::Output: areaLetter = 'Q'; break;
    case Area::Marker: areaLetter = 'M'; break;
    }

    switch (size) {
    case AccessSize::Bit: return fmt::format("%{}X{}.{}", areaLetter, byteOffset, bit);
    case AccessSize::Byte: return fmt::format("%{}B{}", areaLetter, byteOffset);
    case AccessSize::Word: return fmt::format("%{}W{}", areaLetter, byteOffset);
    case AccessSize::DWord: return fmt::format("%{}D{}", areaLetter, byteOffset);
    }
    return {};
}

} // namespace softplc
