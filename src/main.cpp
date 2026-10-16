// soft_plc: runs a Structured Text program, or the built-in C++ blink demo.
//
//   soft_plc                                   blink demo
//   soft_plc examples/traffic_light.st
//   soft_plc --monitor examples/function_blocks.st   ... and accept monitoring connections
//   soft_plc --s7=1102 examples/s7_demo.st           ... and act as an S7 server
#include "core/process_image.hpp"
#include "core/scan_cycle.hpp"
#include "demo/blink_program.hpp"
#include "demo/console_io.hpp"
#include "monitor/monitor_server.hpp"
#include "monitor/monitor_service.hpp"
#include "s7/s7_server.hpp"
#include "st/analyzer.hpp"
#include "st/st_program.hpp"
#include "util/file.hpp"
#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <fmt/format.h>
#include <map>
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
constexpr std::uint16_t kDefaultS7Port = 102;

struct Options {
    std::string programPath;          // empty: run the built-in blink demo
    std::optional<std::uint16_t> monitorPort;
    std::optional<std::uint16_t> s7Port;
    std::map<std::uint16_t, std::size_t> dataBlocks; // --db N:SIZE
};

std::optional<unsigned> parseNumber(std::string_view text, unsigned max) {
    unsigned value = 0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || ptr != text.data() + text.size() || value > max) {
        return std::nullopt;
    }
    return value;
}

// "--name" gives `fallback`; "--name=PORT" gives PORT.
std::optional<std::uint16_t> portOption(std::string_view arg, std::string_view name, std::uint16_t fallback,
                                        bool& matched, bool& valid) {
    matched = false;
    valid = true;
    if (arg == name) {
        matched = true;
        return fallback;
    }
    if (arg.size() > name.size() && arg.substr(0, name.size()) == name && arg[name.size()] == '=') {
        matched = true;
        const auto port = parseNumber(arg.substr(name.size() + 1), 65535);
        valid = port.has_value();
        return port ? std::optional<std::uint16_t>(static_cast<std::uint16_t>(*port)) : std::nullopt;
    }
    return std::nullopt;
}

std::optional<Options> parseOptions(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        bool matched = false;
        bool valid = true;
        if (auto port = portOption(arg, "--monitor", kDefaultMonitorPort, matched, valid); matched) {
            if (!valid) return std::nullopt;
            options.monitorPort = port;
        } else if (auto s7 = portOption(arg, "--s7", kDefaultS7Port, matched, valid); matched) {
            if (!valid) return std::nullopt;
            options.s7Port = s7;
        } else if (arg == "--db" && i + 1 < argc) {
            // --db NUMBER:SIZE, e.g. --db 10:256
            const std::string_view spec = argv[++i];
            const auto colon = spec.find(':');
            const auto number = colon == std::string_view::npos ? std::nullopt : parseNumber(spec.substr(0, colon), 65535);
            const auto size = colon == std::string_view::npos ? std::nullopt : parseNumber(spec.substr(colon + 1), 65536);
            if (!number || !size || *number == 0) {
                return std::nullopt;
            }
            options.dataBlocks[static_cast<std::uint16_t>(*number)] = *size;
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
        fmt::print(stderr, "usage: soft_plc [--monitor[=PORT]] [--s7[=PORT]] [--db N:SIZE]... [program.st]\n"
                           "  --monitor    accept monitoring connections on 127.0.0.1:{} (or PORT);\n"
                           "               needs a program file. Connect with soft_plc_monitor.\n"
                           "  --s7         act as an S7 server on 127.0.0.1:{} (or PORT), e.g. for plc_gui\n"
                           "  --db N:SIZE  create data block N with SIZE bytes (DBs used with AT are\n"
                           "               created automatically, at least as big as needed)\n",
                   kDefaultMonitorPort, kDefaultS7Port);
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

        const bool stayInStop = monitoring || options->s7Port.has_value();
        auto onStop = [&source, stayInStop](const softplc::st::RuntimeError& error) {
            fmt::print(stderr, "PLC STOP: {}\n",
                       softplc::st::formatDiagnostic({error.message, error.location}, source));
            if (!stayInStop) {
                stopRequested = true; // when connected to others, stay up in STOP so they can see it
            }
        };
        // Data blocks: the ones the program uses, at least as big as --db asks for.
        auto dataBlocks = options->dataBlocks;
        for (const auto& [number, size] : softplc::st::dataBlocksUsedBy(*compiled)) {
            dataBlocks[number] = std::max(dataBlocks[number], size);
        }
        for (const auto& [number, size] : dataBlocks) {
            image.addDataBlock(number, size);
        }

        auto st = std::make_unique<softplc::st::StProgram>(std::move(*compiled), onStop);
        stProgram = st.get();
        program = std::move(st);
        fmt::print("Running program '{}' from {}.\n", stProgram->interpreter().program().name, options->programPath);
    } else {
        for (const auto& [number, size] : options->dataBlocks) {
            image.addDataBlock(number, size);
        }
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
        fmt::print("Monitoring on 127.0.0.1:{}.\n", server->port());
    }

    // S7: clients read and write a copy of the memory, exchanged between cycles.
    std::unique_ptr<softplc::s7::S7Server> s7;
    if (options->s7Port) {
        s7 = std::make_unique<softplc::s7::S7Server>(image);
        if (auto started = s7->start(*options->s7Port); !started) {
            fmt::print(stderr, "{}\n", started.error());
            return 1;
        }
        fmt::print("S7 server on 127.0.0.1:{} (rack 0, slot 1), sharing I, Q, M", *options->s7Port);
        for (const auto& [number, bytes] : image.dataBlocks()) {
            fmt::print(", DB{} ({} bytes)", number, bytes.size());
        }
        fmt::print(".\n");
    }

    // Between cycles: answer monitoring requests, then exchange data with S7 clients.
    plc.setIdleTask([&] {
        if (server) {
            server->processPending([&](std::string_view request) { return service->handle(request); });
        }
        if (s7) {
            s7->synchronize();
            s7->setRunning(stProgram == nullptr || !stProgram->stopped());
        }
    });

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
