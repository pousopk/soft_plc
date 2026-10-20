// soft_plc: runs a Structured Text program, always accepting monitoring
// connections (and downloads, e.g. from ladder_editor).
//
//   soft_plc                                   an empty program, waiting for a download
//   soft_plc examples/traffic_light.st
//   soft_plc --monitor=5021 examples/function_blocks.st   ... monitoring on another port
//   soft_plc --s7=1102 examples/s7_demo.st                ... and act as an S7 server
#include "core/process_image.hpp"
#include "core/scan_cycle.hpp"
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

// Prints the value of every variable: every array element and struct field,
// and the variables inside function block instances.
void printVariables(const softplc::st::Interpreter& interpreter, const softplc::ProcessImage& image) {
    using namespace softplc::st;
    fmt::print("Variables:\n");
    for (const auto& variable : interpreter.variables()) {
        const auto value = interpreter.valueOf(variable.path, image);
        fmt::print("  {} : {} = {}\n", variable.path, dataTypeName(variable.type),
                   value ? formatValue(*value, variable.type) : "?");
    }
}

constexpr std::uint16_t kDefaultMonitorPort = 5020;
constexpr std::uint16_t kDefaultS7Port = 102;

struct Options {
    std::string programPath; // empty: start with an empty program
    std::uint16_t monitorPort = kDefaultMonitorPort;
    std::optional<std::uint16_t> s7Port;
    std::map<std::uint16_t, std::size_t> dataBlocks; // --db N:SIZE
    softplc::st::Engine engine = softplc::st::Engine::Bytecode;
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
            options.monitorPort = *port;
        } else if (auto s7 = portOption(arg, "--s7", kDefaultS7Port, matched, valid); matched) {
            if (!valid) return std::nullopt;
            options.s7Port = s7;
        } else if (arg == "--engine=tree") {
            options.engine = softplc::st::Engine::TreeWalker;
        } else if (arg == "--engine=bytecode") {
            options.engine = softplc::st::Engine::Bytecode;
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
    if (!options) {
        fmt::print(stderr, "usage: soft_plc [--monitor=PORT] [--s7[=PORT]] [--db N:SIZE]... [--engine=tree|bytecode]\n"
                           "                [program.st]\n"
                           "  program.st   the program to run; without one, soft_plc starts with an empty\n"
                           "               program and waits for a download (e.g. from ladder_editor)\n"
                           "  --monitor    monitoring (always on) listens on 127.0.0.1:{}, or on PORT.\n"
                           "               Connect with soft_plc_monitor or ladder_editor.\n"
                           "  --s7         act as an S7 server on 127.0.0.1:{} (or PORT), e.g. for plc_gui\n"
                           "  --db N:SIZE  create data block N with SIZE bytes (DBs used with AT are\n"
                           "               created automatically, at least as big as needed)\n"
                           "  --engine     run the program on the bytecode VM (default) or the tree-walker\n",
                   kDefaultMonitorPort, kDefaultS7Port);
        return 2;
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    softplc::ProcessImage image(/*inputBytes=*/16, /*outputBytes=*/16, /*markerBytes=*/256);
    softplc::ConsoleIo io(softplc::Clock::now());

    std::string source{softplc::st::kEmptyProgram};
    if (!options->programPath.empty()) {
        auto contents = softplc::readFile(options->programPath);
        if (!contents) {
            fmt::print(stderr, "cannot open '{}'\n", options->programPath);
            return 1;
        }
        source = std::move(*contents);
    }

    auto compiled = softplc::st::compileProgram(source);
    if (!compiled) {
        fmt::print(stderr, "error: {}\n", softplc::st::formatDiagnostic(compiled.error(), source));
        return 1;
    }

    // A PLC in STOP stays up, so the monitor and S7 clients can see why, and a
    // download can fix it.
    auto onStop = [&source](const softplc::st::RuntimeError& error) {
        fmt::print(stderr, "PLC STOP: {}\n", softplc::st::formatDiagnostic({error.message, error.location}, source));
    };
    // Data blocks: the ones the program uses, at least as big as --db asks for.
    auto dataBlocks = options->dataBlocks;
    for (const auto& [number, size] : softplc::st::dataBlocksUsedBy(*compiled)) {
        dataBlocks[number] = std::max(dataBlocks[number], size);
    }
    for (const auto& [number, size] : dataBlocks) {
        image.addDataBlock(number, size);
    }

    softplc::st::StProgram program(std::move(*compiled), onStop, 1'000'000, options->engine);
    if (options->programPath.empty()) {
        fmt::print("Running an empty program ({} engine): download one from ladder_editor.\n",
                   softplc::st::engineName(options->engine));
    } else {
        fmt::print("Running program '{}' from {} ({} engine).\n", program.interpreter().program().name,
                   options->programPath, softplc::st::engineName(options->engine));
    }

    softplc::ScanCycle plc(image, io, program, 10ms);

    // Monitoring: requests are answered between cycles, on this thread.
    std::unique_ptr<softplc::monitor::MonitorServer> server;
    try {
        server = std::make_unique<softplc::monitor::MonitorServer>(options->monitorPort);
    } catch (const std::system_error& error) {
        fmt::print(stderr, "can't start monitoring on port {}: {} (choose another with --monitor=PORT)\n",
                   options->monitorPort, error.what());
        return 1;
    }
    softplc::monitor::MonitorService service(program, image, plc, source);
    fmt::print("Monitoring on 127.0.0.1:{}.\n", server->port());

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
        server->processPending([&](std::string_view request) { return service.handle(request); });
        if (s7) {
            s7->synchronize();
            s7->setRunning(!program.stopped());
        }
    });

    fmt::print("Cycle time 10 ms. Press Ctrl+C to stop.\n");
    plc.run(stopRequested);

    const auto& stats = plc.stats();
    fmt::print("\nStopped after {} cycles.\n", stats.cycles);
    fmt::print("Execution time: avg {:.1f} us, min {:.1f} us, max {:.1f} us. Overruns: {}\n",
               toMicroseconds(stats.averageExecution()), toMicroseconds(stats.minExecution),
               toMicroseconds(stats.maxExecution), stats.overruns);
    printVariables(program.interpreter(), image);
    return program.stopped() ? 1 : 0;
}
