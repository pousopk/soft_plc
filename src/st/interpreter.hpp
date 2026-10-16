#pragma once
#include "core/process_image.hpp"
#include "st/ast.hpp"
#include "st/diagnostic.hpp"
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace softplc::st {

// An error while the program runs, such as a division by zero. A real PLC
// goes to STOP when this happens.
struct RuntimeError {
    std::string message;
    SourceLocation location;
};

// Runs an analyzed program by walking its syntax tree (a "tree-walking
// interpreter"). Variables keep their values between cycles, like the
// static memory of a PLC program.
//
// Variables declared with AT have no storage of their own: reading or
// writing them reads or writes the process image directly.
class Interpreter {
public:
    // `program` must have passed analyze(). The interpreter takes ownership.
    // `loopLimit` is the watchdog: the most loop iterations allowed per cycle.
    explicit Interpreter(ProgramUnit program, std::uint64_t loopLimit = 1'000'000);

    // Runs the program body once. Throws RuntimeError on errors.
    void runCycle(ProcessImage& image);

    // The current value of a variable, looked up by name (case-insensitive).
    std::optional<Value> valueOf(std::string_view name, const ProcessImage& image) const;
    Value valueOf(std::size_t slot, const ProcessImage& image) const;

    const ProgramUnit& program() const { return program_; }

private:
    // What a statement asks its enclosing statements to do next.
    enum class Flow {
        Normal, // carry on with the next statement
        Exit,   // EXIT: leave the innermost loop
        Return, // RETURN: end this cycle's run of the program
    };

    Flow execute(const StatementList& list);
    Flow execute(const Statement& statement);
    Flow executeFor(const ForStatement& s, SourceLocation where);

    Value evaluate(const Expression& expression);
    Value evaluateBinary(const BinaryExpression& binary, const Expression& expression);

    Value load(std::size_t slot, const ProcessImage& image) const;
    void store(std::size_t slot, const Value& value);

    void countLoopIteration(SourceLocation where);

    ProgramUnit program_;
    std::vector<Value> values_; // one per variable; unused for AT variables
    std::uint64_t loopLimit_;
    std::uint64_t loopIterations_ = 0;
    ProcessImage* image_ = nullptr; // only set while runCycle() runs
};

} // namespace softplc::st
