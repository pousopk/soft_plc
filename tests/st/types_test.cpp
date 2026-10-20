#include <doctest/doctest.h>
#include "st/ast.hpp"
#include "st/runtime_values.hpp"
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

using namespace softplc::st;

TEST_CASE("The new types have names, widths and ranges") {
    CHECK(std::string(dataTypeName(DataType::UDInt)) == "UDINT");
    CHECK(elementaryTypeFromName("word") == DataType::Word);
    CHECK(elementaryTypeFromName("LINT") == DataType::LInt);
    CHECK(bitWidth(DataType::Byte) == 8);
    CHECK(bitWidth(DataType::LInt) == 64);
    CHECK(elementaryType(DataType::SInt)->bitSize == 8);
    CHECK(integerRange(DataType::USInt) == std::pair<std::int64_t, std::int64_t>{0, 255});
    CHECK(integerRange(DataType::SInt) == std::pair<std::int64_t, std::int64_t>{-128, 127});
    CHECK(integerRange(DataType::DWord).second == 4294967295);
    CHECK(isBitString(DataType::Word));
    CHECK_FALSE(isBitString(DataType::Bool));
    CHECK(isUnsignedType(DataType::UInt));
    CHECK_FALSE(isIntegerType(DataType::Word));
}

TEST_CASE("Integers wrap to their type's width") {
    using runtime::wrapInteger;
    CHECK(wrapInteger(256, DataType::USInt) == 0);
    CHECK(wrapInteger(-1, DataType::UInt) == 65535);
    CHECK(wrapInteger(128, DataType::SInt) == -128);
    CHECK(wrapInteger(-1, DataType::DWord) == 4294967295);
    CHECK(wrapInteger(std::numeric_limits<std::int64_t>::min(), DataType::LInt) ==
          std::numeric_limits<std::int64_t>::min());
    CHECK(runtime::toInteger(Value{true}) == 1);
    CHECK(std::get<std::int64_t>(runtime::convert(Value{true}, DataType::Word)) == 1);
}
