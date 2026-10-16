// soft_plc: runs a Structured Text program, or the built-in C++ blink demo.
//
//   soft_plc                          blink demo
//   soft_plc examples/traffic_light.st
#include "core/process_image.hpp"
#include "core/scan_cycle.hpp"
#include "demo/blink_program.hpp"
#include "demo/console_io.hpp"
#include "st/analyzer.hpp"
#include "st/st_program.hpp"
#include "util/file.hpp"
#include <atomic>
#include <chrono>
#include <csignal>
#include <fmt/format.h>
#include <memory>

namespace {

std::atomic<bool> stopRequested{false};

extern "C" void onSignal(int) { stopRequested = true; }

double toMicroseconds(softplc::Duration d) { return std::chrono::duration<double, std::micro>(d).count(); }

// Prints every program variable; for function block instances, their inputs and outputs.
void printVariables(const softplc::st::Interpreter& interpreter, const softplc::ProcessImage& image) {
    using namespace softplc::st;
    const auto& variables = interpreter.program().variables;
    fmt::print("Variables:\n");
    for (std::size_t slot = 0; slot < variables.size(); ++slot) {
        const auto& variable = variables[slot];
        if (variable.type == DataType::FunctionBlock) {
            fmt::print("  {} : {}\n", variable.name, variable.functionBlock->name);
            for (const auto& member : variable.functionBlock->variables) {
                if (member.section == VarSection::Local) {
                    continue;
                }
                const auto value = interpreter.valueOf(variable.name + "." + member.name, image);
                fmt::print("    .{} : {} = {}\n", member.name, dataTypeName(member.type),
                           value ? formatValue(*value, member.type) : "?");
            }
            continue;
        }
        const auto at = variable.address ? " AT " + variable.address->toString() : std::string();
        fmt::print("  {}{} : {} = {}\n", variable.name, at, dataTypeName(variable.type),
                   formatValue(interpreter.valueOf(slot, image), variable.type));
    }
}

} // namespace

int main(int argc, char** argv) {
    using namespace std::chrono_literals;

    if (argc > 2) {
        fmt::print(stderr, "usage: soft_plc [program.st]\n");
        return 2;
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    softplc::ProcessImage image(/*inputBytes=*/16, /*outputBytes=*/16, /*markerBytes=*/256);
    softplc::ConsoleIo io(softplc::Clock::now());

    std::unique_ptr<softplc::Program> program;
    const softplc::st::StProgram* stProgram = nullptr;
    std::string source;

    if (argc == 2) {
        auto contents = softplc::readFile(argv[1]);
        if (!contents) {
            fmt::print(stderr, "cannot open '{}'\n", argv[1]);
            return 1;
        }
        source = std::move(*contents);

        auto compiled = softplc::st::compileProgram(source);
        if (!compiled) {
            fmt::print(stderr, "error: {}\n", softplc::st::formatDiagnostic(compiled.error(), source));
            return 1;
        }

        auto onStop = [&source](const softplc::st::RuntimeError& error) {
            fmt::print(stderr, "PLC STOP: {}\n",
                       softplc::st::formatDiagnostic({error.message, error.location}, source));
            stopRequested = true;
        };
        auto st = std::make_unique<softplc::st::StProgram>(std::move(*compiled), onStop);
        stProgram = st.get();
        program = std::move(st);
        fmt::print("Running program '{}' from {}.\n", stProgram->interpreter().program().name, argv[1]);
    } else {
        program = std::make_unique<softplc::BlinkProgram>();
        fmt::print("Running the built-in blink demo.\n");
    }

    softplc::ScanCycle plc(image, io, *program, 10ms);
    fmt::print("Cycle time 10 ms. Press Ctrl+C to stop.\n");
    plc.run(stopRequested);

    const auto& stats = plc.stats();
    fmt::print("\nStopped after {} cycles.\n", stats.cycles);
    fmt::print("Execution time: avg {:.1f} us, min {:.1f} us, max {:.1f} us. Overruns: {}\n",
               toMicroseconds(stats.averageExecution()), toMicroseconds(stats.minExecution),
               toMicroseconds(stats.maxExecution), stats.overruns);
    if (stProgram != nullptr) {
        printVariables(stProgram->interpreter(), image);
    }
    return stProgram != nullptr && stProgram->stopped() ? 1 : 0;
}
