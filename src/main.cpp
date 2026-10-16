// soft_plc: runs a Structured Text program, or the built-in C++ blink demo.
//
//   soft_plc                                   blink demo
//   soft_plc examples/traffic_light.st
//   soft_plc --monitor examples/function_blocks.st   ... and accept monitoring connections
#include "core/process_image.hpp"
#include "core/scan_cycle.hpp"
#include "demo/blink_program.hpp"
#include "demo/console_io.hpp"
#include "monitor/monitor_server.hpp"
#include "monitor/monitor_service.hpp"
#include "st/analyzer.hpp"
#include "st/st_program.hpp"
#include "util/file.hpp"
#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <fmt/format.h>
#include <memory>
#include <optional>
#include <string_view>
#include <system_error>

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

constexpr std::uint16_t kDefaultMonitorPort = 5020;

struct Options {
    std::string programPath;          // empty: run the built-in blink demo
    std::optional<std::uint16_t> monitorPort;
};

std::optional<Options> parseOptions(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--monitor") {
            options.monitorPort = kDefaultMonitorPort;
        } else if (arg.rfind("--monitor=", 0) == 0) {
            const auto text = arg.substr(std::string_view("--monitor=").size());
            unsigned port = 0;
            const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), port);
            if (ec != std::errc{} || ptr != text.data() + text.size() || port > 65535) {
                return std::nullopt;
            }
            options.monitorPort = static_cast<std::uint16_t>(port);
        } else if (!arg.empty() && arg.front() != '-' && options.programPath.empty()) {
            options.programPath = arg;
        } else {
            return std::nullopt;
        }
    }
    return options;
}

} // namespace

int main(int argc, char** argv) {
    using namespace std::chrono_literals;

    const auto options = parseOptions(argc, argv);
    if (!options || (options->monitorPort && options->programPath.empty())) {
        fmt::print(stderr, "usage: soft_plc [--monitor[=PORT]] [program.st]\n"
                           "  --monitor   accept monitoring connections on 127.0.0.1:{} (or PORT);\n"
                           "              needs a program file. Connect with soft_plc_monitor.\n",
                   kDefaultMonitorPort);
        return 2;
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    softplc::ProcessImage image(/*inputBytes=*/16, /*outputBytes=*/16, /*markerBytes=*/256);
    softplc::ConsoleIo io(softplc::Clock::now());

    std::unique_ptr<softplc::Program> program;
    softplc::st::StProgram* stProgram = nullptr;
    std::string source;
    const bool monitoring = options->monitorPort.has_value();

    if (!options->programPath.empty()) {
        auto contents = softplc::readFile(options->programPath);
        if (!contents) {
            fmt::print(stderr, "cannot open '{}'\n", options->programPath);
            return 1;
        }
        source = std::move(*contents);

        auto compiled = softplc::st::compileProgram(source);
        if (!compiled) {
            fmt::print(stderr, "error: {}\n", softplc::st::formatDiagnostic(compiled.error(), source));
            return 1;
        }

        auto onStop = [&source, monitoring](const softplc::st::RuntimeError& error) {
            fmt::print(stderr, "PLC STOP: {}\n",
                       softplc::st::formatDiagnostic({error.message, error.location}, source));
            if (!monitoring) {
                stopRequested = true; // when monitored, stay up in STOP so the monitor can see why
            }
        };
        auto st = std::make_unique<softplc::st::StProgram>(std::move(*compiled), onStop);
        stProgram = st.get();
        program = std::move(st);
        fmt::print("Running program '{}' from {}.\n", stProgram->interpreter().program().name, options->programPath);
    } else {
        program = std::make_unique<softplc::BlinkProgram>();
        fmt::print("Running the built-in blink demo.\n");
    }

    softplc::ScanCycle plc(image, io, *program, 10ms);

    // Monitoring: requests are answered between cycles, on this thread.
    std::unique_ptr<softplc::monitor::MonitorServer> server;
    std::unique_ptr<softplc::monitor::MonitorService> service;
    if (monitoring) {
        try {
            server = std::make_unique<softplc::monitor::MonitorServer>(*options->monitorPort);
        } catch (const std::system_error& error) {
            fmt::print(stderr, "can't start monitoring on port {}: {}\n", *options->monitorPort, error.what());
            return 1;
        }
        service = std::make_unique<softplc::monitor::MonitorService>(*stProgram, image, plc);
        plc.setIdleTask([&] { server->processPending([&](std::string_view request) { return service->handle(request); }); });
        fmt::print("Monitoring on 127.0.0.1:{}.\n", server->port());
    }

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
