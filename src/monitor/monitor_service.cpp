#include "monitor/monitor_service.hpp"
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

} // namespace

MonitorService::MonitorService(st::StProgram& program, ProcessImage& image, const ScanCycle& cycle)
    : program_(program), image_(image), cycle_(cycle) {}

std::string MonitorService::handle(std::string_view request) {
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
    return fmt::format("ERR unknown command '{}' (try LIST, READ, WRITE, FORCE, UNFORCE, FORCES, STATUS)", command);
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

} // namespace softplc::monitor
