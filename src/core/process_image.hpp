#pragma once
#include "core/address.hpp"
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace softplc {

// The PLC's working memory: input, output and marker areas as raw bytes.
//
// The program never touches physical I/O directly. Each scan cycle, the
// IoDriver copies the real inputs into the input area, the program works
// only on this image, and the IoDriver copies the output area out. That's
// why all inputs look consistent for the whole cycle even if a sensor
// changes in the middle of it.
//
// Words and double words are stored big-endian (most significant byte
// first), the same as Siemens S7, so %MW10 occupies bytes 10 and 11 with
// the high byte in 10. That keeps the layout compatible with Snap7 later.
//
// Every access is bounds-checked and throws std::out_of_range on a bad
// address, the equivalent of a real PLC stopping with an area error.
class ProcessImage {
public:
    ProcessImage(std::size_t inputBytes, std::size_t outputBytes, std::size_t markerBytes);

    // Raw view of an area, for I/O drivers that copy whole areas at once.
    std::span<std::uint8_t> bytes(Area area);
    std::span<const std::uint8_t> bytes(Area area) const;

    bool readBit(Area area, std::size_t byteOffset, unsigned bit) const;
    void writeBit(Area area, std::size_t byteOffset, unsigned bit, bool value);

    std::uint8_t readByte(Area area, std::size_t byteOffset) const;
    void writeByte(Area area, std::size_t byteOffset, std::uint8_t value);

    std::uint16_t readWord(Area area, std::size_t byteOffset) const;
    void writeWord(Area area, std::size_t byteOffset, std::uint16_t value);

    std::uint32_t readDWord(Area area, std::size_t byteOffset) const;
    void writeDWord(Area area, std::size_t byteOffset, std::uint32_t value);

    // Generic access by parsed address, for monitoring tools. The value is
    // widened to 32 bits; a bit address reads as 0 or 1.
    std::uint32_t read(const Address& address) const;
    void write(const Address& address, std::uint32_t value);

private:
    std::vector<std::uint8_t>& storage(Area area);
    const std::vector<std::uint8_t>& storage(Area area) const;

    // Returns the area's bytes after checking [byteOffset, byteOffset + width) fits.
    std::span<const std::uint8_t> checked(Area area, std::size_t byteOffset, std::size_t width) const;
    std::span<std::uint8_t> checked(Area area, std::size_t byteOffset, std::size_t width);

    std::vector<std::uint8_t> inputs_;
    std::vector<std::uint8_t> outputs_;
    std::vector<std::uint8_t> markers_;
};

} // namespace softplc
