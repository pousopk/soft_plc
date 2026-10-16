#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <tl/expected.hpp>

namespace softplc {

// The memory areas of the process image.
enum class Area {
    Input,     // %I — copied from the physical inputs at the start of each cycle
    Output,    // %Q — copied to the physical outputs at the end of each cycle
    Marker,    // %M — internal flags/memory, kept between cycles
    DataBlock, // %DBn — numbered blocks of memory, Siemens style
};

enum class AccessSize {
    Bit,   // X: 1 bit,  e.g. %IX0.3 or %I0.3
    Byte,  // B: 8 bits, e.g. %QB1
    Word,  // W: 16 bits, e.g. %MW10
    DWord, // D: 32 bits, e.g. %MD20
};

// A direct address into the process image: IEC 61131-3 style for I/Q/M
// (%IX0.3, %MW10), Siemens style for data blocks (%DB1.DBX0.3, %DB1.DBW2).
struct Address {
    Area area = Area::Input;
    AccessSize size = AccessSize::Bit;
    std::size_t byteOffset = 0;
    unsigned bit = 0;            // only meaningful when size == AccessSize::Bit
    std::uint16_t dbNumber = 0;  // only meaningful when area == Area::DataBlock

    bool operator==(const Address&) const = default; // C++20: compiler-generated comparison

    // Canonical form, e.g. "%QX0.0", "%MW10" or "%DB1.DBW2".
    std::string toString() const;
};

// Parses "%IX0.3", "%I0.3", "%QB1", "%MW10", "%MD20", and data block
// addresses "%DB1.DBX0.3", "%DB1.DBB2", "%DB1.DBW4", "%DB1.DBD8" (the leading
// '%' is optional). Returns a description of the problem on malformed input.
tl::expected<Address, std::string> parseAddress(std::string_view text);

} // namespace softplc
