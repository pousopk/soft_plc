#include "monitor/monitor_service.hpp"
#include "st/analyzer.hpp"
#include "st/value_text.hpp"
#include "util/text.hpp"
#include <chrono>
#include <fmt/format.h>
#include <sstream>
#include <vector>

namespace softplc::monitor {

namespace {

std::vector<std::string> splitWords(std::string_view text) {
    std::vector<std::string> words;
    std::istringstream stream{std::string(text)};
    for (std::string word; stream >> word;) {
        words.push_back(std::move(word));
    }
    return words;
}

double toMicroseconds(Duration d) { return std::chrono::duration<double, std::micro>(d).count(); }

// LOAD's source: \n is a line end, \\ a backslash.
std::string unescape(std::string_view text) {
    std::string result;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\\' && i + 1 < text.size()) {
            ++i;
            result += text[i] == 'n' ? '\n' : text[i];
        } else {
            result += text[i];
        }
    }
    return result;
}

} // namespace

MonitorService::MonitorService(st::StProgram& program, ProcessImage& image, const ScanCycle& cycle)
    : program_(program), image_(image), cycle_(cycle) {}

std::string MonitorService::handle(std::string_view request) {
    // LOAD's argument is a whole program, spaces and all: not split in words.
    if (request.size() > 5 && toUpper(request.substr(0, 5)) == "LOAD ") {
        return load(request.substr(5));
    }
    auto words = splitWords(request);
    if (words.empty()) {
        return "ERR empty request";
    }
    const auto command = toUpper(words.front());
    words.erase(words.begin());

    if (command == "LIST") return list();
    if (command == "READ") return read(words);
    if (command == "WRITE") return write(words);
    if (command == "FORCE") return force(words);
    if (command == "UNFORCE") return unforce(words);
    if (command == "FORCES") return forces();
    if (command == "STATUS") return status();
    return fmt::format("ERR unknown command '{}' (try LIST, READ, WRITE, FORCE, UNFORCE, FORCES, STATUS, LOAD)", command);
}

std::string MonitorService::list() const {
    std::string reply = "OK";
    for (const auto& variable : program_.interpreter().variables()) {
        reply += fmt::format(" {}:{}", variable.path, st::dataTypeName(variable.type));
    }
    return reply;
}

std::string MonitorService::read(const std::vector<std::string>& paths) const {
    if (paths.empty()) {
        return "ERR READ needs at least one variable";
    }
    const auto& interpreter = program_.interpreter();
    std::string reply = "OK";
    for (const auto& path : paths) {
        const auto variable = interpreter.describe(path);
        const auto value = interpreter.valueOf(path, image_);
        reply += ' ';
        reply += variable && value ? st::formatValue(*value, variable->type) : "?";
    }
    return reply;
}

std::string MonitorService::write(const std::vector<std::string>& args) {
    if (args.size() != 2) {
        return "ERR usage: WRITE <variable> <value>";
    }
    const auto variable = program_.interpreter().describe(args[0]);
    if (!variable) {
        return fmt::format("ERR unknown variable '{}'", args[0]);
    }
    const auto value = st::parseValue(args[1], variable->type);
    if (!value) {
        return "ERR " + value.error();
    }
    program_.interpreter().setValue(variable->path, *value, image_);
    return "OK";
}

std::string MonitorService::force(const std::vector<std::string>& args) {
    if (args.size() != 2) {
        return "ERR usage: FORCE <variable> <value>";
    }
    const auto variable = program_.interpreter().describe(args[0]);
    if (!variable) {
        return fmt::format("ERR unknown variable '{}'", args[0]);
    }
    const auto value = st::parseValue(args[1], variable->type);
    if (!value) {
        return "ERR " + value.error();
    }
    program_.force(variable->path, *value);
    // Apply it right away too, so a READ straight after shows the forced value.
    program_.interpreter().setValue(variable->path, *value, image_);
    return "OK";
}

std::string MonitorService::unforce(const std::vector<std::string>& args) {
    if (args.size() != 1) {
        return "ERR usage: UNFORCE <variable>";
    }
    if (!program_.unforce(args[0])) {
        return fmt::format("ERR '{}' is not forced", args[0]);
    }
    return "OK";
}

std::string MonitorService::forces() const {
    std::string reply = "OK";
    for (const auto& [path, value] : program_.forces()) {
        const auto variable = program_.interpreter().describe(path);
        reply += fmt::format(" {}={}", path, st::formatValue(value, variable ? variable->type : st::DataType::Unknown));
    }
    return reply;
}

std::string MonitorService::status() const {
    const auto& stats = cycle_.stats();
    const auto cycleMs = std::chrono::duration<double, std::milli>(cycle_.cycleTime()).count();
    auto reply = fmt::format("OK {} cycles={} cycle_ms={} avg_us={:.1f} max_us={:.1f} overruns={}",
                             program_.stopped() ? "STOP" : "RUN", stats.cycles, cycleMs,
                             toMicroseconds(stats.averageExecution()), toMicroseconds(stats.maxExecution),
                             stats.overruns);
    if (const auto& error = program_.stopError()) {
        // The error goes last because it contains spaces: everything after "error=" is the message.
        reply += fmt::format(" error=line {}, column {}: {}", error->location.line, error->location.column,
                             error->message);
    }
    return reply;
}

std::string MonitorService::load(std::string_view escapedSource) {
    const auto source = unescape(escapedSource);
    auto unit = st::compileProgram(source);
    if (!unit) {
        return fmt::format("ERR line {}, column {}: {}", unit.error().location.line, unit.error().location.column,
                           unit.error().message);
    }
    // Data blocks can be added while running, but not resized: their bytes
    // are what S7 clients and AT variables see.
    for (const auto& [number, size] : st::dataBlocksUsedBy(*unit)) {
        if (!image_.hasDataBlock(number)) {
            image_.addDataBlock(number, size);
        } else if (image_.dataBlock(number).size() < size) {
            return fmt::format("ERR DB{} would need {} bytes, but it has {}: restart soft_plc to change its size",
                               number, size, image_.dataBlock(number).size());
        }
    }
    const auto result = program_.replace(std::move(*unit));
    auto reply = fmt::format("OK kept={} initialized={}", result.kept, result.initialized.size());
    for (const auto& path : result.initialized) {
        reply += ' ' + path;
    }
    return reply;
}

} // namespace softplc::monitor
