#pragma once
#include "core/address.hpp"
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <vector>

namespace softplc {

// The PLC's working memory: input, output and marker areas, plus numbered
// data blocks, all as raw bytes.
//
// The program never touches physical I/O directly. Each scan cycle, the
// IoDriver copies the real inputs into the input area, the program works
// only on this image, and the IoDriver copies the output area out. That's
// why all inputs look consistent for the whole cycle even if a sensor
// changes in the middle of it.
//
// Words and double words are stored big-endian (most significant byte
// first), the same as Siemens S7, so %MW10 occupies bytes 10 and 11 with
// the high byte in 10. That keeps the layout compatible with S7 clients.
//
// Every access is bounds-checked and throws std::out_of_range on a bad
// address, the equivalent of a real PLC stopping with an area error.
//
// The bytes of an area or data block never move once created (nothing is
// ever resized), so their addresses can be handed to code like the S7
// server that keeps pointers to them.
class ProcessImage {
public:
    ProcessImage(std::size_t inputBytes, std::size_t outputBytes, std::size_t markerBytes);

    // Raw view of an area, for I/O drivers that copy whole areas at once.
    // Not for Area::DataBlock: use dataBlock() for those.
    std::span<std::uint8_t> bytes(Area area);
    std::span<const std::uint8_t> bytes(Area area) const;

    // Data blocks. Each one is created once, with a fixed size, filled with zeros.
    // Throws std::invalid_argument if the block already exists or the number is 0.
    void addDataBlock(std::uint16_t number, std::size_t size);
    bool hasDataBlock(std::uint16_t number) const;
    std::span<std::uint8_t> dataBlock(std::uint16_t number);             // throws std::out_of_range if missing
    std::span<const std::uint8_t> dataBlock(std::uint16_t number) const; // throws std::out_of_range if missing
    const std::map<std::uint16_t, std::vector<std::uint8_t>>& dataBlocks() const { return dataBlocks_; }

    bool readBit(Area area, std::size_t byteOffset, unsigned bit) const;
    void writeBit(Area area, std::size_t byteOffset, unsigned bit, bool value);

    std::uint8_t readByte(Area area, std::size_t byteOffset) const;
    void writeByte(Area area, std::size_t byteOffset, std::uint8_t value);

    std::uint16_t readWord(Area area, std::size_t byteOffset) const;
    void writeWord(Area area, std::size_t byteOffset, std::uint16_t value);

    std::uint32_t readDWord(Area area, std::size_t byteOffset) const;
    void writeDWord(Area area, std::size_t byteOffset, std::uint32_t value);

    // Generic access by address, for any area including data blocks. The
    // value is widened to 32 bits; a bit address reads as 0 or 1.
    std::uint32_t read(const Address& address) const;
    void write(const Address& address, std::uint32_t value);

private:
    std::vector<std::uint8_t>& storage(Area area);
    const std::vector<std::uint8_t>& storage(Area area) const;

    // The bytes an address points into: an area, or one data block.
    std::span<std::uint8_t> region(const Address& address);
    std::span<const std::uint8_t> region(const Address& address) const;

    std::vector<std::uint8_t> inputs_;
    std::vector<std::uint8_t> outputs_;
    std::vector<std::uint8_t> markers_;
    std::map<std::uint16_t, std::vector<std::uint8_t>> dataBlocks_;
};

} // namespace softplc
