#include <doctest/doctest.h>
#include "core/address.hpp"

using namespace softplc;

TEST_CASE("parseAddress reads bit addresses with and without the X size letter") {
    auto explicitBit = parseAddress("%IX0.3");
    REQUIRE(explicitBit.has_value());
    CHECK(*explicitBit == Address{Area::Input, AccessSize::Bit, 0, 3});

    auto shortBit = parseAddress("%Q12.7");
    REQUIRE(shortBit.has_value());
    CHECK(*shortBit == Address{Area::Output, AccessSize::Bit, 12, 7});
}

TEST_CASE("parseAddress reads byte, word and double word addresses") {
    CHECK(parseAddress("%QB1").value() == Address{Area::Output, AccessSize::Byte, 1, 0});
    CHECK(parseAddress("%MW10").value() == Address{Area::Marker, AccessSize::Word, 10, 0});
    CHECK(parseAddress("%MD20").value() == Address{Area::Marker, AccessSize::DWord, 20, 0});
}

TEST_CASE("parseAddress accepts addresses without the leading %") {
    CHECK(parseAddress("MW4").value() == Address{Area::Marker, AccessSize::Word, 4, 0});
}

TEST_CASE("parseAddress rejects malformed addresses with a reason") {
    const char* bad[] = {"", "%", "%Z0.0", "%I", "%I0", "%I0.8", "%IX0.", "%MW", "%MW10.1", "%QB1x", "%I-1.0"};
    for (const char* text : bad) {
        CAPTURE(text);
        auto result = parseAddress(text);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().find(text) != std::string::npos); // the message names the bad input
    }
}

TEST_CASE("toString produces the canonical form and round-trips through parseAddress") {
    const Address addresses[] = {
        {Area::Input, AccessSize::Bit, 0, 3},
        {Area::Output, AccessSize::Byte, 1, 0},
        {Area::Marker, AccessSize::Word, 10, 0},
        {Area::Marker, AccessSize::DWord, 20, 0},
    };
    CHECK(addresses[0].toString() == "%IX0.3");
    CHECK(addresses[3].toString() == "%MD20");
    for (const auto& address : addresses) {
        CHECK(parseAddress(address.toString()).value() == address);
    }
}
