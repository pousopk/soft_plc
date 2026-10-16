#include "core/process_image.hpp"
#include <fmt/format.h>
#include <stdexcept>
#include <string>
#include <utility>

namespace softplc {

namespace {

std::string regionName(Area area, std::uint16_t dbNumber) {
    switch (area) {
    case Area::Input: return "input area";
    case Area::Output: return "output area";
    case Area::Marker: return "marker area";
    case Area::DataBlock: return fmt::format("DB{}", dbNumber);
    }
    return "unknown area";
}

void checkBit(unsigned bit) {
    if (bit > 7) {
        throw std::out_of_range(fmt::format("bit number {} is out of range (0-7)", bit));
    }
}

void checkRange(std::span<const std::uint8_t> mem, std::size_t byteOffset, std::size_t width, const std::string& name) {
    if (byteOffset > mem.size() || width > mem.size() - byteOffset) {
        throw std::out_of_range(fmt::format("access to {} bytes {}..{} is outside the {} ({} bytes)", width,
                                            byteOffset, byteOffset + width - 1, name, mem.size()));
    }
}

// The actual reads and writes, on any span of bytes. Big-endian, like S7.

bool getBit(std::span<const std::uint8_t> mem, std::size_t offset, unsigned bit) {
    return ((mem[offset] >> bit) & 1) != 0;
}

void setBit(std::span<std::uint8_t> mem, std::size_t offset, unsigned bit, bool value) {
    const auto mask = static_cast<std::uint8_t>(1u << bit);
    mem[offset] = value ? static_cast<std::uint8_t>(mem[offset] | mask) : static_cast<std::uint8_t>(mem[offset] & ~mask);
}

std::uint16_t getWord(std::span<const std::uint8_t> mem, std::size_t offset) {
    return static_cast<std::uint16_t>((mem[offset] << 8) | mem[offset + 1]);
}

void setWord(std::span<std::uint8_t> mem, std::size_t offset, std::uint16_t value) {
    mem[offset] = static_cast<std::uint8_t>(value >> 8);
    mem[offset + 1] = static_cast<std::uint8_t>(value);
}

std::uint32_t getDWord(std::span<const std::uint8_t> mem, std::size_t offset) {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        value = (value << 8) | mem[offset + i];
    }
    return value;
}

void setDWord(std::span<std::uint8_t> mem, std::size_t offset, std::uint32_t value) {
    for (std::size_t i = 0; i < 4; ++i) {
        mem[offset + i] = static_cast<std::uint8_t>(value >> (24 - 8 * i));
    }
}

std::size_t widthOf(AccessSize size) {
    switch (size) {
    case AccessSize::Bit:
    case AccessSize::Byte: return 1;
    case AccessSize::Word: return 2;
    case AccessSize::DWord: return 4;
    }
    return 1;
}

} // namespace

ProcessImage::ProcessImage(std::size_t inputBytes, std::size_t outputBytes, std::size_t markerBytes)
    : inputs_(inputBytes), outputs_(outputBytes), markers_(markerBytes) {}

std::span<std::uint8_t> ProcessImage::bytes(Area area) { return storage(area); }

std::span<const std::uint8_t> ProcessImage::bytes(Area area) const { return storage(area); }

void ProcessImage::addDataBlock(std::uint16_t number, std::size_t size) {
    if (number == 0) {
        throw std::invalid_argument("data block numbers start at 1");
    }
    if (!dataBlocks_.emplace(number, std::vector<std::uint8_t>(size)).second) {
        throw std::invalid_argument(fmt::format("DB{} already exists", number));
    }
}

bool ProcessImage::hasDataBlock(std::uint16_t number) const { return dataBlocks_.count(number) > 0; }

std::span<const std::uint8_t> ProcessImage::dataBlock(std::uint16_t number) const {
    const auto it = dataBlocks_.find(number);
    if (it == dataBlocks_.end()) {
        throw std::out_of_range(fmt::format("DB{} doesn't exist", number));
    }
    return it->second;
}

std::span<std::uint8_t> ProcessImage::dataBlock(std::uint16_t number) {
    const auto view = std::as_const(*this).dataBlock(number);
    return {const_cast<std::uint8_t*>(view.data()), view.size()}; // safe: *this isn't const here
}

bool ProcessImage::readBit(Area area, std::size_t byteOffset, unsigned bit) const {
    return read(Address{area, AccessSize::Bit, byteOffset, bit}) != 0;
}

void ProcessImage::writeBit(Area area, std::size_t byteOffset, unsigned bit, bool value) {
    write(Address{area, AccessSize::Bit, byteOffset, bit}, value ? 1u : 0u);
}

std::uint8_t ProcessImage::readByte(Area area, std::size_t byteOffset) const {
    return static_cast<std::uint8_t>(read(Address{area, AccessSize::Byte, byteOffset}));
}

void ProcessImage::writeByte(Area area, std::size_t byteOffset, std::uint8_t value) {
    write(Address{area, AccessSize::Byte, byteOffset}, value);
}

std::uint16_t ProcessImage::readWord(Area area, std::size_t byteOffset) const {
    return static_cast<std::uint16_t>(read(Address{area, AccessSize::Word, byteOffset}));
}

void ProcessImage::writeWord(Area area, std::size_t byteOffset, std::uint16_t value) {
    write(Address{area, AccessSize::Word, byteOffset}, value);
}

std::uint32_t ProcessImage::readDWord(Area area, std::size_t byteOffset) const {
    return read(Address{area, AccessSize::DWord, byteOffset});
}

void ProcessImage::writeDWord(Area area, std::size_t byteOffset, std::uint32_t value) {
    write(Address{area, AccessSize::DWord, byteOffset}, value);
}

std::uint32_t ProcessImage::read(const Address& address) const {
    const auto mem = region(address);
    if (address.size == AccessSize::Bit) {
        checkBit(address.bit);
    }
    checkRange(mem, address.byteOffset, widthOf(address.size), regionName(address.area, address.dbNumber));
    switch (address.size) {
    case AccessSize::Bit: return getBit(mem, address.byteOffset, address.bit) ? 1u : 0u;
    case AccessSize::Byte: return mem[address.byteOffset];
    case AccessSize::Word: return getWord(mem, address.byteOffset);
    case AccessSize::DWord: return getDWord(mem, address.byteOffset);
    }
    return 0;
}

void ProcessImage::write(const Address& address, std::uint32_t value) {
    const auto mem = region(address);
    if (address.size == AccessSize::Bit) {
        checkBit(address.bit);
    }
    checkRange(mem, address.byteOffset, widthOf(address.size), regionName(address.area, address.dbNumber));
    switch (address.size) {
    case AccessSize::Bit: setBit(mem, address.byteOffset, address.bit, value != 0); break;
    case AccessSize::Byte: mem[address.byteOffset] = static_cast<std::uint8_t>(value); break;
    case AccessSize::Word: setWord(mem, address.byteOffset, static_cast<std::uint16_t>(value)); break;
    case AccessSize::DWord: setDWord(mem, address.byteOffset, value); break;
    }
}

const std::vector<std::uint8_t>& ProcessImage::storage(Area area) const {
    switch (area) {
    case Area::Input: return inputs_;
    case Area::Output: return outputs_;
    case Area::Marker: return markers_;
    case Area::DataBlock: break;
    }
    throw std::invalid_argument("data blocks are numbered: use dataBlock(number)");
}

std::vector<std::uint8_t>& ProcessImage::storage(Area area) {
    // Reuse the const version; casting away const on the result is safe
    // because *this is not const here.
    return const_cast<std::vector<std::uint8_t>&>(std::as_const(*this).storage(area));
}

std::span<const std::uint8_t> ProcessImage::region(const Address& address) const {
    if (address.area == Area::DataBlock) {
        return dataBlock(address.dbNumber);
    }
    return storage(address.area);
}

std::span<std::uint8_t> ProcessImage::region(const Address& address) {
    if (address.area == Area::DataBlock) {
        return dataBlock(address.dbNumber);
    }
    return storage(address.area);
}

} // namespace softplc
