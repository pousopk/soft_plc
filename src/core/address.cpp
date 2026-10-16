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
        return fail(text, "missing area (I, Q, M or DB)");
    }
    if (rest.substr(0, 2) == "DB") {
        // Siemens data block address: DB<number>.DB<size letter><offset>[.<bit>]
        rest.remove_prefix(2);
        std::size_t number = 0;
        if (!consumeNumber(rest, number) || number == 0 || number > 65535) {
            return fail(text, "data block number must be 1-65535");
        }
        if (rest.substr(0, 3) != ".DB") {
            return fail(text, "a data block address looks like %DB1.DBX0.0, %DB1.DBW2 or %DB1.DBD4");
        }
        rest.remove_prefix(3);
        address.area = Area::DataBlock;
        address.dbNumber = static_cast<std::uint16_t>(number);
        if (rest.empty()) {
            return fail(text, "missing size letter after .DB (X, B, W or D)");
        }
    } else {
        switch (rest.front()) {
        case 'I': address.area = Area::Input; break;
        case 'Q': address.area = Area::Output; break;
        case 'M': address.area = Area::Marker; break;
        default: return fail(text, "area must be I, Q, M or DB");
        }
        rest.remove_prefix(1);
    }

    // The size letter is optional for I/Q/M; without one, the address is a bit
    // address. Data blocks always have one (DBX, DBB, DBW, DBD).
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
        } else if (address.area == Area::DataBlock) {
            return fail(text, "size letter after .DB must be X, B, W or D");
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
    std::string prefix;
    switch (area) {
    case Area::Input: prefix = "%I"; break;
    case Area::Output: prefix = "%Q"; break;
    case Area::Marker: prefix = "%M"; break;
    case Area::DataBlock: prefix = fmt::format("%DB{}.DB", dbNumber); break;
    }

    switch (size) {
    case AccessSize::Bit: return fmt::format("{}X{}.{}", prefix, byteOffset, bit);
    case AccessSize::Byte: return fmt::format("{}B{}", prefix, byteOffset);
    case AccessSize::Word: return fmt::format("{}W{}", prefix, byteOffset);
    case AccessSize::DWord: return fmt::format("{}D{}", prefix, byteOffset);
    }
    return {};
}

} // namespace softplc
