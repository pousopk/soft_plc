#pragma once
#include "core/plc_time.hpp"
#include "core/process_image.hpp"
#include "st/ast.hpp"
#include "st/bytecode.hpp"
#include "st/diagnostic.hpp"
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace softplc::st {

// An error while the program runs, such as a division by zero. A real PLC
// goes to STOP when this happens.
struct RuntimeError {
    std::string message;
    SourceLocation location;
};

// One elementary variable, anywhere in the instance tree.
struct VariableInfo {
    std::string path; // "count", "fast.out", "fast.offTimer.ET", spelled as declared
    DataType type;
};

// How the program's code is run. Both give exactly the same results.
enum class Engine {
    TreeWalker, // walk the syntax tree directly (milestone 3)
    Bytecode,   // compile to bytecode once, then run it on a stack VM (milestone 7)
};

const char* engineName(Engine engine); // "tree", "bytecode"

// One slot of the VM's stack: an integer (also BOOL 0/1 and TIME in ms) or a
// REAL. Untagged: the compiler always knows which one is in use.
union VmSlot {
    std::int64_t i;
    double r;
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
    explicit Interpreter(CompilationUnit unit, std::uint64_t loopLimit = 1'000'000, Engine engine = Engine::Bytecode);

    // Declared here, defined in the .cpp where Instance is complete: a
    // unique_ptr can only delete a type the compiler fully knows.
    ~Interpreter();
    Interpreter(Interpreter&&) noexcept;
    Interpreter& operator=(Interpreter&&) noexcept;

    // Runs the program body once. `now` is the cycle's timestamp, which the
    // timers use. Throws RuntimeError on errors.
    void runCycle(ProcessImage& image, TimePoint now);

    // Every elementary variable of the program and, recursively, of every
    // function block instance inside it, including their internal variables.
    // This is what a monitoring tool can see (it may see more than ST code can).
    std::vector<VariableInfo> variables() const;

    // Looks up a variable by path, case-insensitively: "count", "t1.Q",
    // "fast.offTimer.ET". Gives its spelling as declared, and its type.
    std::optional<VariableInfo> describe(std::string_view path) const;

    // The current value of a variable, by path (see describe()).
    std::optional<Value> valueOf(std::string_view path, const ProcessImage& image) const;

    // Writes a variable from outside the program, the way a monitoring tool
    // does. The value is converted to the variable's type (it must be of the
    // right kind: a bool for BOOL, an integer for INT/DINT/TIME, a number for
    // REAL). Returns false if there's no such variable.
    bool setValue(std::string_view path, const Value& value, ProcessImage& image);

    const Pou& program() const { return unit_->program; }
    Engine engine() const { return engine_; }

    // The bytecode of a POU (only with Engine::Bytecode), e.g. for st_parse --bytecode.
    const Chunk* bytecode(const Pou& pou) const;

private:
    // A place in memory that holds values: one variable of one instance,
    // plus an offset inside it (an array element, a struct field). For AT
    // variables the offset counts bits in the process image; for the rest it
    // counts leaves (see Type).
    struct Place {
        Instance* instance = nullptr;
        const VariableDeclaration* variable = nullptr;
        std::size_t leaf = 0;
        std::size_t bit = 0;
    };

    // A FUNCTION's memory. A function can't call itself, so it never runs
    // twice at once, and one frame each is enough. It's reset to `initial`
    // at every call: functions remember nothing between calls.
    struct FunctionFrame {
        std::unique_ptr<Instance> instance;
        std::vector<Value> initial;
    };

    // One argument of a FUNCTION call, worked out before the call.
    struct Argument {
        std::size_t slot = 0;              // the input
        const Type* aggregate = nullptr;   // an array or struct: copied from `from`
        Value value;                       // otherwise: the value
        Place from;
    };

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
    Value callFunction(const CallExpression& call, Instance& self);
    Value callBuiltin(const CallExpression& call, Instance& self, SourceLocation where);
    Value invokeFunction(std::size_t index, std::span<const Argument> arguments);
    Value evaluateBinary(const BinaryExpression& binary, const Expression& expression, Instance& self);

    void initialize(Instance& instance, const Type& type, std::size_t leaf, const Initializer* initializer);

    // Where an access points to, with its run-time indices worked out and
    // checked (shared by both engines, so both report the same errors).
    Place place(const Access& access, Instance& self);
    static Place wholeVariable(Instance& instance, std::size_t slot);
    Value loadLeaf(const Place& place, DataType type, const ProcessImage* image) const;
    void storeLeaf(const Place& place, DataType type, const Value& value);
    void copyAggregate(const Place& from, const Place& to, const Type& type);

    void countLoopIteration(SourceLocation where);

    // The bytecode VM: runs one POU's compiled code on `self`.
    void runBytecode(const Chunk& chunk, Instance& self);
    Place vmPlace(const PlaceInfo& info, Instance& self, std::int64_t offset) const;

    // On the heap, so that the instances' pointers into it stay valid when
    // the Interpreter itself is moved.
    std::unique_ptr<CompilationUnit> unit_;
    std::unique_ptr<Instance> root_; // the PROGRAM's instance
    std::uint64_t loopLimit_;
    std::uint64_t loopIterations_ = 0;
    ProcessImage* image_ = nullptr; // only set while runCycle() runs
    TimePoint now_{};

    Engine engine_;
    std::unordered_map<const Pou*, Chunk> chunks_; // compiled code, per POU (Bytecode engine)
    std::vector<FunctionFrame> functions_;         // one per CompilationUnit::functions
    // The VM's stack, shared by nested function block calls: each call uses
    // the part above its caller's. Fixed size, so pointers into it stay valid.
    std::vector<VmSlot> vmStack_;
    std::size_t vmTop_ = 0;
};

} // namespace softplc::st
