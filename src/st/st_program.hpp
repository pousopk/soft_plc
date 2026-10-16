#pragma once
#include "core/program.hpp"
#include "st/interpreter.hpp"
#include <functional>
#include <optional>

namespace softplc::st {

// Plugs a Structured Text program into the scan cycle (it implements the same
// Program interface as the C++ demo programs).
//
// On a runtime error it behaves like a PLC going to STOP: the program stops
// executing, all outputs are switched off (the safe state), and the optional
// `onStop` callback is told why.
class StProgram : public Program {
public:
    using StopHandler = std::function<void(const RuntimeError&)>;

    explicit StProgram(ProgramUnit program, StopHandler onStop = {}, std::uint64_t loopLimit = 1'000'000);

    void execute(ProcessImage& image, const CycleContext& context) override;

    bool stopped() const { return stopError_.has_value(); }
    const std::optional<RuntimeError>& stopError() const { return stopError_; }
    const Interpreter& interpreter() const { return interpreter_; }

private:
    Interpreter interpreter_;
    StopHandler onStop_;
    std::optional<RuntimeError> stopError_;
};

} // namespace softplc::st
