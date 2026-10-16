#pragma once
#include "core/plc_time.hpp"
#include "core/process_image.hpp"
#include "st/ast.hpp"
#include "st/diagnostic.hpp"
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace softplc::st {

// An error while the program runs, such as a division by zero. A real PLC
// goes to STOP when this happens.
struct RuntimeError {
    std::string message;
    SourceLocation location;
};

// The memory of one PROGRAM or function block instance. Defined in
// interpreter.cpp: nothing outside the interpreter needs to see inside it.
struct Instance;

// Runs an analyzed program by walking its syntax tree (a "tree-walking
// interpreter").
//
// The program is one instance; every function block variable inside it is
// another instance, with its own copy of that block's variables, and so on
// down. All of them keep their values between cycles, like the static memory
// of a PLC.
//
// Variables declared with AT have no storage of their own: reading or
// writing them reads or writes the process image directly.
class Interpreter {
public:
    // `unit` must have passed analyze(). The interpreter takes ownership.
    // `loopLimit` is the watchdog: the most loop iterations allowed per cycle.
    explicit Interpreter(CompilationUnit unit, std::uint64_t loopLimit = 1'000'000);

    // Declared here, defined in the .cpp where Instance is complete: a
    // unique_ptr can only delete a type the compiler fully knows.
    ~Interpreter();
    Interpreter(Interpreter&&) noexcept;
    Interpreter& operator=(Interpreter&&) noexcept;

    // Runs the program body once. `now` is the cycle's timestamp, which the
    // timers use. Throws RuntimeError on errors.
    void runCycle(ProcessImage& image, TimePoint now);

    // The current value of a program variable, by name (case-insensitive),
    // or of an instance member: "count" or "t1.Q".
    std::optional<Value> valueOf(std::string_view name, const ProcessImage& image) const;

    // The current value of a program variable by slot (not for instances).
    Value valueOf(std::size_t slot, const ProcessImage& image) const;

    const Pou& program() const { return unit_->program; }

private:
    // What a statement asks its enclosing statements to do next.
    enum class Flow {
        Normal, // carry on with the next statement
        Exit,   // EXIT: leave the innermost loop
        Return, // RETURN: end this POU's run
    };

    std::unique_ptr<Instance> createInstance(const Pou& pou);
    void run(Instance& instance);

    Flow execute(const StatementList& list, Instance& self);
    Flow execute(const Statement& statement, Instance& self);
    Flow executeFor(const ForStatement& s, SourceLocation where, Instance& self);
    void call(const CallStatement& s, Instance& self);

    Value evaluate(const Expression& expression, Instance& self);
    Value evaluateBinary(const BinaryExpression& binary, const Expression& expression, Instance& self);

    Value load(const Instance& self, std::size_t slot, const ProcessImage* image) const;
    void store(Instance& self, std::size_t slot, const Value& value);

    void countLoopIteration(SourceLocation where);

    // On the heap, so that the instances' pointers into it stay valid when
    // the Interpreter itself is moved.
    std::unique_ptr<CompilationUnit> unit_;
    std::unique_ptr<Instance> root_; // the PROGRAM's instance
    std::uint64_t loopLimit_;
    std::uint64_t loopIterations_ = 0;
    ProcessImage* image_ = nullptr; // only set while runCycle() runs
    TimePoint now_{};
};

} // namespace softplc::st
