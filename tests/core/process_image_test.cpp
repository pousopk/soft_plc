#include <doctest/doctest.h>
#include "core/process_image.hpp"
#include <stdexcept>

using namespace softplc;

TEST_CASE("ProcessImage starts with every area zeroed and sized as requested") {
    ProcessImage image(2, 4, 8);
    CHECK(image.bytes(Area::Input).size() == 2);
    CHECK(image.bytes(Area::Output).size() == 4);
    CHECK(image.bytes(Area::Marker).size() == 8);
    for (auto byte : image.bytes(Area::Marker)) {
        CHECK(byte == 0);
    }
}

TEST_CASE("writeBit sets and clears one bit without touching its neighbours") {
    ProcessImage image(1, 1, 1);
    image.writeByte(Area::Output, 0, 0b1010'0000);

    image.writeBit(Area::Output, 0, 0, true);
    CHECK(image.readByte(Area::Output, 0) == 0b1010'0001);
    CHECK(image.readBit(Area::Output, 0, 0));
    CHECK(image.readBit(Area::Output, 0, 7));
    CHECK_FALSE(image.readBit(Area::Output, 0, 6));

    image.writeBit(Area::Output, 0, 7, false);
    CHECK(image.readByte(Area::Output, 0) == 0b0010'0001);
}

TEST_CASE("Areas are independent") {
    ProcessImage image(1, 1, 1);
    image.writeByte(Area::Marker, 0, 0xFF);
    CHECK(image.readByte(Area::Input, 0) == 0);
    CHECK(image.readByte(Area::Output, 0) == 0);
}

TEST_CASE("Words and double words are stored big-endian, like Siemens S7") {
    ProcessImage image(0, 0, 8);
    image.writeWord(Area::Marker, 0, 0x1234);
    CHECK(image.readByte(Area::Marker, 0) == 0x12);
    CHECK(image.readByte(Area::Marker, 1) == 0x34);
    CHECK(image.readWord(Area::Marker, 0) == 0x1234);

    image.writeDWord(Area::Marker, 4, 0xDEADBEEF);
    CHECK(image.readByte(Area::Marker, 4) == 0xDE);
    CHECK(image.readByte(Area::Marker, 7) == 0xEF);
    CHECK(image.readDWord(Area::Marker, 4) == 0xDEADBEEF);
}

TEST_CASE("read and write by Address dispatch on the access size") {
    ProcessImage image(0, 0, 8);
    image.write(parseAddress("%MW2").value(), 0xABCD);
    CHECK(image.readWord(Area::Marker, 2) == 0xABCD);
    CHECK(image.read(parseAddress("%MB2").value()) == 0xAB);
    CHECK(image.read(parseAddress("%M3.0").value()) == 1); // 0xCD = 1100'1101
    CHECK(image.read(parseAddress("%M3.1").value()) == 0);
}

TEST_CASE("Out-of-range access throws instead of touching other memory") {
    ProcessImage image(2, 2, 2);
    CHECK_THROWS_AS(image.readByte(Area::Input, 2), std::out_of_range);
    CHECK_THROWS_AS(image.readWord(Area::Input, 1), std::out_of_range); // second byte would be past the end
    CHECK_THROWS_AS(image.writeDWord(Area::Marker, 0, 1), std::out_of_range);
    CHECK_THROWS_AS(image.readBit(Area::Output, 0, 8), std::out_of_range);
    CHECK_NOTHROW(image.readWord(Area::Input, 0));
}
