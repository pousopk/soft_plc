// st_bench: runs a program on both engines as fast as possible (no waiting
// between cycles) and compares how long a cycle takes.
//
//   st_bench examples/benchmark.st            10'000 cycles on each engine
//   st_bench examples/benchmark.st 50000
//
// Use a Release build for meaningful numbers: the Debug build's sanitizers
// slow everything down several times.
#include "st/analyzer.hpp"
#include "st/interpreter.hpp"
#include "util/file.hpp"
#include <charconv>
#include <chrono>
#include <fmt/format.h>
#include <string>
#include <string_view>

namespace {

using namespace softplc;
using namespace std::chrono_literals;

struct Result {
    double microsecondsPerCycle = 0;
    std::string finalState; // every variable, to check both engines ended up the same
};

Result run(const std::string& source, st::Engine engine, int cycles) {
    auto unit = st::compileProgram(source).value();
    ProcessImage image(64, 64, 1024);
    for (const auto& [number, size] : st::dataBlocksUsedBy(unit)) {
        image.addDataBlock(number, size);
    }
    st::Interpreter interpreter(std::move(unit), 100'000'000, engine);

    auto now = TimePoint{} + 1h; // a simulated clock: 10 ms per cycle, like the real scan cycle
    for (int i = 0; i < cycles / 10; ++i) { // warm-up: caches, branch predictors, first allocations
        interpreter.runCycle(image, now);
        now += 10ms;
    }

    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < cycles; ++i) {
        interpreter.runCycle(image, now);
        now += 10ms;
    }
    const std::chrono::duration<double, std::micro> elapsed = std::chrono::steady_clock::now() - start;

    Result result;
    result.microsecondsPerCycle = elapsed.count() / cycles;
    for (const auto& variable : interpreter.variables()) {
        result.finalState += variable.path + "=" + *interpreter.formatValueOf(variable.path, image) + " ";
    }
    return result;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) {
        fmt::print(stderr, "usage: st_bench <program.st> [cycles]\n");
        return 2;
    }
    int cycles = 10'000;
    if (argc == 3) {
        const std::string_view text = argv[2];
        const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), cycles);
        if (ec != std::errc{} || cycles <= 0) {
            fmt::print(stderr, "cycles must be a positive number\n");
            return 2;
        }
    }

    const auto source = readFile(argv[1]);
    if (!source) {
        fmt::print(stderr, "cannot open '{}'\n", argv[1]);
        return 1;
    }
    if (auto check = st::compileProgram(*source); !check) {
        fmt::print(stderr, "error: {}\n", st::formatDiagnostic(check.error(), *source));
        return 1;
    }

    try {
        const auto tree = run(*source, st::Engine::TreeWalker, cycles);
        const auto bytecode = run(*source, st::Engine::Bytecode, cycles);

        fmt::print("{} cycles of {}\n", cycles, argv[1]);
        fmt::print("  tree-walking interpreter: {:8.2f} us per cycle\n", tree.microsecondsPerCycle);
        fmt::print("  bytecode VM:              {:8.2f} us per cycle\n", bytecode.microsecondsPerCycle);
        fmt::print("  speed-up: {:.2f}x\n", tree.microsecondsPerCycle / bytecode.microsecondsPerCycle);
        if (tree.finalState != bytecode.finalState) {
            fmt::print(stderr, "WARNING: the engines ended in different states!\n");
            return 1;
        }
        fmt::print("  both engines ended with identical variables\n");
    } catch (const st::RuntimeError& error) {
        fmt::print(stderr, "runtime error: {}\n", st::formatDiagnostic({error.message, error.location}, *source));
        return 1;
    }
    return 0;
}
