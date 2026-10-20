#include <doctest/doctest.h>
#include "st/string_pool.hpp"
#include "st/string_text.hpp"
#include <array>
#include <string>

using namespace softplc::st;

namespace {

std::u16string formatted(const Value& value, DataType type, std::size_t room = 64) {
    std::array<char16_t, 64> buffer{};
    const auto length = formatInto(std::span(buffer.data(), room), value, type);
    return std::u16string(buffer.data(), length);
}

} // namespace

TEST_CASE("A string pool holds fixed-size buffers and cuts what doesn't fit") {
    StringPool pool;
    const auto a = pool.add(3);
    const auto b = pool.add(10);
    CHECK(pool.capacity(a) == 3);
    CHECK(pool.view(a).empty());
    pool.assign(a, u"abcdef");
    CHECK(pool.view(a) == u"abc");
    pool.assign(b, u"hello");
    pool.assign(b, pool.view(b).substr(1)); // its own text, shifted
    CHECK(pool.view(b) == u"ello");
    pool.setChar(b, 0, u'j');
    CHECK(pool.view(b) == u"jllo");
    pool.clear(b);
    CHECK(pool.view(b).empty());
}

TEST_CASE("Literal text: $ escapes and UTF-8") {
    CHECK(decodeLiteral("It$'s$N$$", TextKind::String).value() == u"It's\n$");
    CHECK(decodeLiteral("$41$l$R$t$P", TextKind::String).value() == u"A\n\r\t\f");
    CHECK(decodeLiteral("\xC3\xA9", TextKind::String).value() == u"é");
    CHECK(decodeLiteral("$0041$\"", TextKind::WString).value() == u"A\"");
    CHECK(decodeLiteral("\xE2\x82\xAC", TextKind::WString).value() == u"€");
    CHECK(decodeLiteral("\xE2\x82\xAC", TextKind::String).error() ==
          "character '\xE2\x82\xAC' doesn't fit in a STRING: write it as $hh");
    CHECK(decodeLiteral("$q", TextKind::String).error() == "unknown escape '$q' in a string");
    CHECK(decodeLiteral("\xC3", TextKind::String).error() == "invalid UTF-8 in a string");
}

TEST_CASE("Text written back as a literal: quotes, escapes and plain ASCII") {
    CHECK(encodeLiteral(u"It's é", TextKind::String) == "'It$'s $E9'");
    CHECK(encodeLiteral(u"a b", TextKind::String) == "'a b'");
    CHECK(encodeLiteral(u"a\n$", TextKind::String) == "'a$0A$$'");
    CHECK(encodeLiteral(u"€\"", TextKind::WString) == "\"$20AC$\"\"");
}

TEST_CASE("Texts compare character by character, a prefix first") {
    CHECK(compareText(u"abc", u"abd") < 0);
    CHECK(compareText(u"ab", u"abc") < 0);
    CHECK(compareText(u"b", u"abc") > 0);
    CHECK(compareText(u"", u"") == 0);
}

TEST_CASE("Values become text without allocating, as the monitor shows them") {
    CHECK(formatted(Value{std::int64_t{-5}}, DataType::Int) == u"-5");
    CHECK(formatted(Value{std::int64_t{255}}, DataType::Word) == u"16#00FF");
    CHECK(formatted(Value{2.5}, DataType::Real) == u"2.5");
    CHECK(formatted(Value{true}, DataType::Bool) == u"TRUE");
    CHECK(formatted(Value{std::int64_t{1500}}, DataType::Time) == u"T#1s500ms");
    CHECK(formatted(Value{std::int64_t{255}}, DataType::Word, 3) == u"16#");
    for (const auto& [value, type] : {std::pair{Value{std::int64_t{-32768}}, DataType::Int},
                                      std::pair{Value{1.0e-7}, DataType::Real},
                                      std::pair{Value{std::int64_t{-90061001}}, DataType::Time},
                                      std::pair{Value{std::int64_t{4294967295}}, DataType::DWord}}) {
        const auto text = formatValue(value, type);
        CHECK(formatted(value, type) == std::u16string(text.begin(), text.end()));
    }
}

TEST_CASE("Text becomes a value, or nothing if it isn't one") {
    CHECK(std::get<std::int64_t>(*parseText(u"42", DataType::Int)) == 42);
    CHECK(std::get<std::int64_t>(*parseText(u"-7", DataType::SInt)) == -7);
    CHECK(std::get<std::int64_t>(*parseText(u"16#FF", DataType::Byte)) == 255);
    CHECK(std::get<std::int64_t>(*parseText(u"2#1010", DataType::Word)) == 10);
    CHECK(std::get<double>(*parseText(u"2.5", DataType::Real)) == 2.5);
    CHECK(std::get<double>(*parseText(u"-3", DataType::Real)) == -3.0);
    CHECK(std::get<std::int64_t>(*parseText(u"T#2s", DataType::Time)) == 2000);
    CHECK(std::get<std::int64_t>(*parseText(u"T#1m30s", DataType::Time)) == 90000);
    CHECK(std::get<bool>(*parseText(u"true", DataType::Bool)));
    CHECK_FALSE(parseText(u"x", DataType::Int).has_value());
    CHECK_FALSE(parseText(u"70000", DataType::Int).has_value());
    CHECK_FALSE(parseText(u"4é", DataType::Int).has_value());
    CHECK_FALSE(parseText(u"", DataType::Real).has_value());
}
