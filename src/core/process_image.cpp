#include "core/process_image.hpp"
#include <fmt/format.h>
#include <stdexcept>
#include <utility>

namespace softplc {

namespace {

const char* areaName(Area area) {
    switch (area) {
    case Area::Input: return "input";
    case Area::Output: return "output";
    case Area::Marker: return "marker";
    }
    return "unknown";
}

void checkBit(unsigned bit) {
    if (bit > 7) {
        throw std::out_of_range(fmt::format("bit number {} is out of range (0-7)", bit));
    }
}

} // namespace

ProcessImage::ProcessImage(std::size_t inputBytes, std::size_t outputBytes, std::size_t markerBytes)
    : inputs_(inputBytes), outputs_(outputBytes), markers_(markerBytes) {}

std::span<std::uint8_t> ProcessImage::bytes(Area area) { return storage(area); }

std::span<const std::uint8_t> ProcessImage::bytes(Area area) const { return storage(area); }

bool ProcessImage::readBit(Area area, std::size_t byteOffset, unsigned bit) const {
    checkBit(bit);
    const auto mem = checked(area, byteOffset, 1);
    return ((mem[byteOffset] >> bit) & 1) != 0;
}

void ProcessImage::writeBit(Area area, std::size_t byteOffset, unsigned bit, bool value) {
    checkBit(bit);
    auto mem = checked(area, byteOffset, 1);
    const auto mask = static_cast<std::uint8_t>(1u << bit);
    if (value) {
        mem[byteOffset] = static_cast<std::uint8_t>(mem[byteOffset] | mask);
    } else {
        mem[byteOffset] = static_cast<std::uint8_t>(mem[byteOffset] & ~mask);
    }
}

std::uint8_t ProcessImage::readByte(Area area, std::size_t byteOffset) const {
    return checked(area, byteOffset, 1)[byteOffset];
}

void ProcessImage::writeByte(Area area, std::size_t byteOffset, std::uint8_t value) {
    checked(area, byteOffset, 1)[byteOffset] = value;
}

std::uint16_t ProcessImage::readWord(Area area, std::size_t byteOffset) const {
    const auto mem = checked(area, byteOffset, 2);
    return static_cast<std::uint16_t>((mem[byteOffset] << 8) | mem[byteOffset + 1]);
}

void ProcessImage::writeWord(Area area, std::size_t byteOffset, std::uint16_t value) {
    auto mem = checked(area, byteOffset, 2);
    mem[byteOffset] = static_cast<std::uint8_t>(value >> 8);
    mem[byteOffset + 1] = static_cast<std::uint8_t>(value);
}

std::uint32_t ProcessImage::readDWord(Area area, std::size_t byteOffset) const {
    const auto mem = checked(area, byteOffset, 4);
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        value = (value << 8) | mem[byteOffset + i];
    }
    return value;
}

void ProcessImage::writeDWord(Area area, std::size_t byteOffset, std::uint32_t value) {
    auto mem = checked(area, byteOffset, 4);
    for (std::size_t i = 0; i < 4; ++i) {
        mem[byteOffset + i] = static_cast<std::uint8_t>(value >> (24 - 8 * i));
    }
}

std::uint32_t ProcessImage::read(const Address& address) const {
    switch (address.size) {
    case AccessSize::Bit: return readBit(address.area, address.byteOffset, address.bit) ? 1u : 0u;
    case AccessSize::Byte: return readByte(address.area, address.byteOffset);
    case AccessSize::Word: return readWord(address.area, address.byteOffset);
    case AccessSize::DWord: return readDWord(address.area, address.byteOffset);
    }
    return 0;
}

void ProcessImage::write(const Address& address, std::uint32_t value) {
    switch (address.size) {
    case AccessSize::Bit: writeBit(address.area, address.byteOffset, address.bit, value != 0); break;
    case AccessSize::Byte: writeByte(address.area, address.byteOffset, static_cast<std::uint8_t>(value)); break;
    case AccessSize::Word: writeWord(address.area, address.byteOffset, static_cast<std::uint16_t>(value)); break;
    case AccessSize::DWord: writeDWord(address.area, address.byteOffset, value); break;
    }
}

const std::vector<std::uint8_t>& ProcessImage::storage(Area area) const {
    switch (area) {
    case Area::Input: return inputs_;
    case Area::Output: return outputs_;
    case Area::Marker: return markers_;
    }
    throw std::invalid_argument("unknown area");
}

std::vector<std::uint8_t>& ProcessImage::storage(Area area) {
    // Reuse the const version; casting away const on the result is safe
    // because *this is not const here.
    return const_cast<std::vector<std::uint8_t>&>(std::as_const(*this).storage(area));
}

std::span<const std::uint8_t> ProcessImage::checked(Area area, std::size_t byteOffset, std::size_t width) const {
    const auto& mem = storage(area);
    if (byteOffset > mem.size() || width > mem.size() - byteOffset) {
        throw std::out_of_range(fmt::format("access to {} bytes {}..{} is outside the {} area ({} bytes)", width,
                                            byteOffset, byteOffset + width - 1, areaName(area), mem.size()));
    }
    return mem;
}

std::span<std::uint8_t> ProcessImage::checked(Area area, std::size_t byteOffset, std::size_t width) {
    std::as_const(*this).checked(area, byteOffset, width);
    return storage(area);
}

} // namespace softplc
