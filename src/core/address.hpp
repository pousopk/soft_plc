#pragma once
#include <cstddef>
#include <string>
#include <string_view>
#include <tl/expected.hpp>

namespace softplc {

// The three memory areas of the process image.
enum class Area {
    Input,  // %I — copied from the physical inputs at the start of each cycle
    Output, // %Q — copied to the physical outputs at the end of each cycle
    Marker, // %M — internal flags/memory, kept between cycles
};

enum class AccessSize {
    Bit,   // X: 1 bit,  e.g. %IX0.3 or %I0.3
    Byte,  // B: 8 bits, e.g. %QB1
    Word,  // W: 16 bits, e.g. %MW10
    DWord, // D: 32 bits, e.g. %MD20
};

// A direct address into the process image, IEC 61131-3 style.
struct Address {
    Area area = Area::Input;
    AccessSize size = AccessSize::Bit;
    std::size_t byteOffset = 0;
    unsigned bit = 0; // only meaningful when size == AccessSize::Bit

    bool operator==(const Address&) const = default; // C++20: compiler-generated comparison

    // Canonical form, e.g. "%QX0.0" or "%MW10".
    std::string toString() const;
};

// Parses "%IX0.3", "%I0.3", "%QB1", "%MW10", "%MD20" (the leading '%' is
// optional). Returns a description of the problem on malformed input.
tl::expected<Address, std::string> parseAddress(std::string_view text);

} // namespace softplc
