#include <doctest/doctest.h>
#include "st/analyzer.hpp"
#include "st/interpreter.hpp"
#include "support/engine.hpp"
#include <chrono>
#include <string>

using namespace softplc;
using namespace softplc::st;

// RuntimeError doesn't derive from std::exception, so tell doctest how to show it.
REGISTER_EXCEPTION_TRANSLATOR(const RuntimeError& error) { return doctest::String(error.message.c_str()); }

namespace {

// Compiles and runs a program, then reads variables, texts included.
class Runner {
public:
    explicit Runner(const std::string& source) : image_(8, 8, 64), interpreter_(compile(source, image_), 1'000'000, testEngine()) {}

    Runner& cycles(int count) {
        for (int i = 0; i < count; ++i) {
            interpreter_.runCycle(image_, now_);
            now_ += std::chrono::milliseconds(10);
        }
        return *this;
    }

    std::u16string text(std::string_view path) const {
        auto t = interpreter_.textOf(path, image_);
        REQUIRE_MESSAGE(t.has_value(), "no text " << std::string(path));
        return *t;
    }
    std::int64_t integer(std::string_view path) const { return std::get<std::int64_t>(value(path)); }
    bool boolean(std::string_view path) const { return std::get<bool>(value(path)); }

    ProcessImage& image() { return image_; }

private:
    static CompilationUnit compile(const std::string& source, ProcessImage& image) {
        auto unit = compileProgram(source);
        if (!unit) {
            FAIL("compile error: " << unit.error().message);
        }
        for (const auto& [number, size] : dataBlocksUsedBy(*unit)) {
            image.addDataBlock(number, size);
        }
        return std::move(*unit);
    }

    Value value(std::string_view path) const {
        auto v = interpreter_.valueOf(path, image_);
        REQUIRE_MESSAGE(v.has_value(), "no variable " << std::string(path));
        return *v;
    }

    ProcessImage image_;
    Interpreter interpreter_;
    TimePoint now_ = TimePoint{} + std::chrono::seconds(100);
};

} // namespace

TEST_CASE("Strings are assigned by copying, cut to the target's length") {
    Runner r("PROGRAM P VAR s : STRING[5] := 'ab'; t : STRING; a, b : STRING; w : WSTRING[3]; big : STRING := 'hello world';\n"
             "wide : WSTRING := \"$20ACabcd\"; END_VAR\n"
             "t := s; s := big; a := 'x'; b := a; a := 'y'; w := wide;\nEND_PROGRAM");
    r.cycles(1);
    CHECK(r.text("s") == u"hello");
    CHECK(r.text("t") == u"ab");
    CHECK(r.text("a") == u"y");
    CHECK(r.text("b") == u"x");
    CHECK(r.text("w") == u"€ab");
}

TEST_CASE("Strings and characters compare by their codes") {
    Runner r("PROGRAM P VAR s : STRING := 'hello'; c : CHAR := 'a'; wc : WCHAR; f1, f2, f3, f4, f5 : BOOL; END_VAR\n"
             "f1 := 'abc' < 'abd'; f2 := 'ab' < 'abc'; f3 := s = 'hello'; f4 := c < 'b'; wc := \"z\"; "
             "f5 := s <> 'hello' OR wc <> \"z\";\nEND_PROGRAM");
    r.cycles(1);
    CHECK(r.boolean("f1"));
    CHECK(r.boolean("f2"));
    CHECK(r.boolean("f3"));
    CHECK(r.boolean("f4"));
    CHECK_FALSE(r.boolean("f5"));
    CHECK(r.text("c") == u"a");
    CHECK(r.integer("c") == 97);
}

TEST_CASE("Arrays and structs and function blocks hold strings") {
    Runner r("TYPE Recipe : STRUCT name : STRING[8] := 'none'; step : INT; END_STRUCT; END_TYPE\n"
             "FUNCTION_BLOCK Greeter\n"
             "VAR_INPUT name : STRING[10]; END_VAR VAR_OUTPUT greeting : STRING; END_VAR\n"
             "    greeting := name;\n"
             "END_FUNCTION_BLOCK\n"
             "PROGRAM P VAR names : ARRAY[1..2] OF STRING[8] := ['one', 'two']; copy : ARRAY[1..2] OF STRING[8];\n"
             "    r1, r2 : Recipe; g : Greeter; out : STRING; long : STRING := 'a very long name'; END_VAR\n"
             "    copy := names; names[1] := 'uno'; r2 := r1; r1.name := 'bread';\n"
             "    g(name := long); out := g.greeting;\nEND_PROGRAM");
    r.cycles(1);
    CHECK(r.text("names[1]") == u"uno");
    CHECK(r.text("copy[1]") == u"one");
    CHECK(r.text("copy[2]") == u"two");
    CHECK(r.text("r1.name") == u"bread");
    CHECK(r.text("r2.name") == u"none");
    CHECK(r.text("out") == u"a very lon");
}

TEST_CASE("A FUNCTION returning a STRING: each call keeps its own result, and locals start over") {
    Runner r("FUNCTION Pick : STRING\n"
             "VAR_INPUT first : BOOL; END_VAR VAR tmp : STRING := 'x'; END_VAR\n"
             "    IF first THEN Pick := tmp; tmp := 'changed'; ELSE tmp := 'y'; Pick := tmp; END_IF;\n"
             "END_FUNCTION\n"
             "PROGRAM P VAR same : BOOL := TRUE; a, b : STRING; END_VAR\n"
             "    same := Pick(TRUE) = Pick(FALSE); a := Pick(TRUE); b := Pick(FALSE);\nEND_PROGRAM");
    r.cycles(1);
    CHECK_FALSE(r.boolean("same"));
    CHECK(r.text("a") == u"x");
    CHECK(r.text("b") == u"y");
}

TEST_CASE("Strings placed with AT use the Siemens layout") {
    Runner r("PROGRAM P VAR m AT %MB0 : STRING[4]; w AT %MB10 : WSTRING[2]; c AT %MB20 : CHAR; END_VAR\n"
             "m := 'hi'; w := \"A\"; c := 'Z';\nEND_PROGRAM");
    r.cycles(1);
    const auto& image = r.image();
    CHECK(image.readByte(Area::Marker, 0) == 4);
    CHECK(image.readByte(Area::Marker, 1) == 2);
    CHECK(image.readByte(Area::Marker, 2) == 'h');
    CHECK(image.readByte(Area::Marker, 3) == 'i');
    CHECK(image.readWord(Area::Marker, 10) == 2);
    CHECK(image.readWord(Area::Marker, 12) == 1);
    CHECK(image.readWord(Area::Marker, 14) == 0x41);
    CHECK(image.readByte(Area::Marker, 20) == 'Z');
    CHECK(r.text("m") == u"hi");
}

TEST_CASE("A located string with a bad header is read cut to its length, and its header is set") {
    Runner r("PROGRAM P VAR m AT %MB0 : STRING[4]; copy : STRING; END_VAR\ncopy := m;\nEND_PROGRAM");
    for (std::size_t i = 0; i < 6; ++i) {
        r.image().writeByte(Area::Marker, i, static_cast<std::uint8_t>(std::string_view("\x07\x09" "abcd")[i]));
    }
    r.cycles(1);
    CHECK(r.text("copy") == u"abcd");
    CHECK(r.image().readByte(Area::Marker, 0) == 4);
}
