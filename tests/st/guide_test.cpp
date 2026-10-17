#include "st/analyzer.hpp"
#include "st/interpreter.hpp"
#include "support/engine.hpp"
#include "util/file.hpp"
#include <algorithm>
#include <chrono>
#include <doctest/doctest.h>
#include <filesystem>
#include <string>
#include <vector>

using namespace softplc;
using namespace softplc::st;

// Keeps the programming guide (docs/guide) honest: every complete program in
// it must compile and run, and every example marked
//     (* error: <message> *)
// on its first line must fail to compile with exactly that message.

namespace {

struct Example {
    std::string file;
    std::size_t line; // where the code block starts, for failure messages
    std::string code;
};

// The ```st code blocks of a Markdown file that contain a whole program.
std::vector<Example> programsIn(const std::filesystem::path& path) {
    const auto text = readFile(path.string());
    REQUIRE(text.has_value());
    std::vector<Example> examples;
    std::size_t lineNumber = 0;
    std::size_t blockStart = 0;
    bool inBlock = false;
    std::string block;
    std::size_t begin = 0;
    while (begin <= text->size()) {
        const auto end = std::min(text->find('\n', begin), text->size());
        const auto line = text->substr(begin, end - begin);
        ++lineNumber;
        if (!inBlock && line == "```st") {
            inBlock = true;
            blockStart = lineNumber;
            block.clear();
        } else if (inBlock && line == "```") {
            inBlock = false;
            if (block.find("END_PROGRAM") != std::string::npos) {
                examples.push_back(Example{path.filename().string(), blockStart, block});
            }
        } else if (inBlock) {
            block += line + "\n";
        }
        begin = end + 1;
    }
    return examples;
}

} // namespace

TEST_CASE("Every program in the programming guide compiles and runs, and every error example fails as shown") {
    const std::filesystem::path guide = std::filesystem::path(SOFT_PLC_SOURCE_DIR) / "docs" / "guide";
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(guide)) {
        if (entry.path().extension() == ".md") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());

    int programs = 0;
    int errors = 0;
    for (const auto& file : files) {
        for (const auto& example : programsIn(file)) {
            CAPTURE(example.file);
            CAPTURE(example.line);
            auto unit = compileProgram(example.code);

            const std::string marker = "(* error: ";
            if (example.code.rfind(marker, 0) == 0) {
                const auto messageEnd = example.code.find(" *)");
                const auto expected = example.code.substr(marker.size(), messageEnd - marker.size());
                REQUIRE_FALSE(unit.has_value());
                CHECK(unit.error().message == expected);
                ++errors;
                continue;
            }

            REQUIRE_MESSAGE(unit.has_value(), (unit ? "" : unit.error().message));
            ProcessImage image(16, 16, 256); // the same areas as soft_plc
            for (const auto& [number, size] : dataBlocksUsedBy(*unit)) {
                image.addDataBlock(number, size);
            }
            Interpreter interpreter(std::move(*unit), 1'000'000, testEngine());
            auto now = TimePoint{} + std::chrono::hours(1);
            for (int cycle = 0; cycle < 20; ++cycle) {
                CHECK_NOTHROW(interpreter.runCycle(image, now));
                now += std::chrono::milliseconds(10);
            }
            ++programs;
        }
    }
    // A guard against the extraction silently finding nothing.
    CHECK(programs >= 20);
    CHECK(errors >= 4);
    MESSAGE(programs << " programs and " << errors << " error examples checked");
}
