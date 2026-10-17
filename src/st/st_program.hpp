#pragma once
#include "core/program.hpp"
#include "st/interpreter.hpp"
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace softplc::st {

// Plugs a Structured Text program into the scan cycle (it implements the same
// Program interface as the C++ demo programs).
//
// On a runtime error it behaves like a PLC going to STOP: the program stops
// executing, all outputs are switched off (the safe state), and the optional
// `onStop` callback is told why.
//
// It also supports *forcing*, as used during commissioning: a forced variable
// is held at a fixed value whatever the program does. The value is written
// just before the program runs (so the program reads it) and again just after
// (so the program's own writes are overridden before the outputs go out).
class StProgram : public Program {
public:
    using StopHandler = std::function<void(const RuntimeError&)>;

    explicit StProgram(CompilationUnit unit, StopHandler onStop = {}, std::uint64_t loopLimit = 1'000'000,
                       Engine engine = Engine::Bytecode);

    void execute(ProcessImage& image, const CycleContext& context) override;

    bool stopped() const { return stopError_.has_value(); }
    const std::optional<RuntimeError>& stopError() const { return stopError_; }

    const Interpreter& interpreter() const { return interpreter_; }
    Interpreter& interpreter() { return interpreter_; }

    // Online change: from the next cycle on, runs `unit` instead, keeping
    // the values of every variable it shares with the running program (see
    // Interpreter::takeOver). Forces on variables that no longer exist are
    // dropped. A program in STOP goes back to RUN: the change may be its fix.
    // Call it between cycles only.
    Interpreter::Takeover replace(CompilationUnit unit);

    // Forces a variable (by path, as in Interpreter::describe) to `value`.
    // Returns false if there's no such variable.
    bool force(std::string_view path, const Value& value);
    // Removes a force. Returns false if the variable wasn't forced.
    bool unforce(std::string_view path);
    // The forced variables, keyed by their path as declared.
    const std::map<std::string, Value>& forces() const { return forces_; }

private:
    void applyForces(ProcessImage& image);
    void stop(ProcessImage& image, const RuntimeError& error);

    std::uint64_t loopLimit_;
    Interpreter interpreter_;
    StopHandler onStop_;
    std::optional<RuntimeError> stopError_;
    std::map<std::string, Value> forces_;
};

} // namespace softplc::st
