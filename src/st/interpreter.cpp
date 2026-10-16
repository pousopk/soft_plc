#include "st/interpreter.hpp"
#include "st/builtins.hpp"
#include "util/overloaded.hpp"
#include "util/text.hpp"
#include <bit>
#include <cmath>
#include <utility>
#include <fmt/format.h>
#include <vector>

namespace softplc::st {

// The memory of one POU instance.
struct Instance {
    const Pou* type = nullptr;
    // One entry per variable of `type`, in declaration order. Unused for
    // function block variables (see `children`) and AT variables (they live
    // in the process image).
    std::vector<Value> values;
    // One entry per variable; set only for function block variables.
    std::vector<std::unique_ptr<Instance>> children;
    // Set only for standard blocks like TON: their behaviour in C++.
    std::unique_ptr<NativeBlock> native;
};

namespace {

// --- Converting values to the shape of their type ---

std::int64_t toInteger(const Value& value) { return std::get<std::int64_t>(value); }

double toReal(const Value& value) {
    if (const auto* i = std::get_if<std::int64_t>(&value)) {
        return static_cast<double>(*i);
    }
    return std::get<double>(value);
}

// REAL is a 32-bit float: values are held in a double, but rounded to float
// precision after every operation so results match a real PLC.
double roundToReal(double value) { return static_cast<double>(static_cast<float>(value)); }

// Brings an integer into its type's range by wrapping around, exactly like a
// PLC does: INT 32767 + 1 gives -32768. Since C++20, converting to a smaller
// signed type is guaranteed to wrap this way.
std::int64_t wrapInteger(std::int64_t value, DataType type) {
    switch (type) {
    case DataType::Int: return static_cast<std::int16_t>(value);
    case DataType::DInt:
    case DataType::Time: return static_cast<std::int32_t>(value);
    default: return value; // AnyInt: a plain 64-bit calculation of literals
    }
}

// Converts a value to the given type, for storing in a variable.
Value convert(const Value& value, DataType type) {
    switch (type) {
    case DataType::Bool: return std::get<bool>(value);
    case DataType::Real: return roundToReal(toReal(value));
    default: return wrapInteger(toInteger(value), type);
    }
}

Value defaultValue(DataType type) {
    switch (type) {
    case DataType::Bool: return false;
    case DataType::Real: return 0.0;
    default: return std::int64_t{0};
    }
}

// --- 64-bit integer arithmetic without undefined behaviour ---
// Signed overflow is undefined behaviour in C++, so + - * are done on
// unsigned numbers (which wrap) and converted back.

std::int64_t add(std::int64_t a, std::int64_t b) {
    return static_cast<std::int64_t>(static_cast<std::uint64_t>(a) + static_cast<std::uint64_t>(b));
}
std::int64_t subtract(std::int64_t a, std::int64_t b) {
    return static_cast<std::int64_t>(static_cast<std::uint64_t>(a) - static_cast<std::uint64_t>(b));
}
std::int64_t multiply(std::int64_t a, std::int64_t b) {
    return static_cast<std::int64_t>(static_cast<std::uint64_t>(a) * static_cast<std::uint64_t>(b));
}

// Applies a comparison to two values of the same kind (bools, integers or reals).
template <class T> bool compare(BinaryOperator op, T a, T b) {
    switch (op) {
    case BinaryOperator::Equal: return a == b;
    case BinaryOperator::NotEqual: return a != b;
    case BinaryOperator::Less: return a < b;
    case BinaryOperator::LessEqual: return a <= b;
    case BinaryOperator::Greater: return a > b;
    case BinaryOperator::GreaterEqual: return a >= b;
    default: return false;
    }
}

bool isComparison(BinaryOperator op) {
    return op == BinaryOperator::Equal || op == BinaryOperator::NotEqual || op == BinaryOperator::Less ||
           op == BinaryOperator::LessEqual || op == BinaryOperator::Greater || op == BinaryOperator::GreaterEqual;
}

std::optional<std::size_t> findVariable(const Pou& pou, std::string_view name) {
    const auto upper = toUpper(name);
    for (std::size_t slot = 0; slot < pou.variables.size(); ++slot) {
        if (toUpper(pou.variables[slot].name) == upper) {
            return slot;
        }
    }
    return std::nullopt;
}

// Connects the interpreter to a process image for the duration of one cycle,
// and disconnects it again when the cycle ends, even if it ends with an
// exception. This is RAII: the destructor does the cleanup.
class ImageBinding {
public:
    ImageBinding(ProcessImage*& slot, ProcessImage& image) : slot_(slot) { slot_ = &image; }
    ~ImageBinding() { slot_ = nullptr; }
    ImageBinding(const ImageBinding&) = delete;
    ImageBinding& operator=(const ImageBinding&) = delete;

private:
    ProcessImage*& slot_;
};

} // namespace

Interpreter::Interpreter(CompilationUnit unit, std::uint64_t loopLimit)
    : unit_(std::make_unique<CompilationUnit>(std::move(unit))), loopLimit_(loopLimit) {
    root_ = createInstance(unit_->program);
}

Interpreter::~Interpreter() = default;
Interpreter::Interpreter(Interpreter&&) noexcept = default;
Interpreter& Interpreter::operator=(Interpreter&&) noexcept = default;

// Builds the memory for one instance of `pou`, and recursively for every
// function block instance inside it.
std::unique_ptr<Instance> Interpreter::createInstance(const Pou& pou) {
    auto instance = std::make_unique<Instance>();
    instance->type = &pou;
    instance->children.resize(pou.variables.size());
    instance->values.reserve(pou.variables.size());
    for (std::size_t slot = 0; slot < pou.variables.size(); ++slot) {
        const auto& variable = pou.variables[slot];
        if (variable.type == DataType::FunctionBlock) {
            instance->children[slot] = createInstance(*variable.functionBlock);
            instance->values.push_back(Value{}); // placeholder, never used
        } else if (variable.initialValue) {
            // Initial values are constants (the analyzer checked), so this
            // doesn't need a process image or any other variable.
            instance->values.push_back(convert(evaluate(*variable.initialValue, *instance), variable.type));
        } else {
            instance->values.push_back(defaultValue(variable.type));
        }
    }
    if (pou.builtin) {
        instance->native = createBuiltin(pou.name);
    }
    return instance;
}

void Interpreter::runCycle(ProcessImage& image, TimePoint now) {
    ImageBinding binding(image_, image);
    now_ = now;
    loopIterations_ = 0;
    run(*root_);
}

// Runs one instance's code once: the C++ behaviour of a standard block, or
// the ST body of a PROGRAM or user function block.
void Interpreter::run(Instance& instance) {
    if (instance.native) {
        instance.native->execute(instance.values, now_);
    } else {
        execute(instance.type->body, instance); // a RETURN just ends this run early
    }
}

namespace {

// A variable found by path: the instance that holds it, its slot there, and
// its spelling as declared.
template <class InstanceType> struct Resolved {
    InstanceType* instance;
    std::size_t slot;
    std::string path;
};

// Walks "a.b.c" down the instance tree: every part but the last must be a
// function block instance, and the last must be an elementary variable.
//
// A template, so the same code works on a `const Instance` (for reading) and
// on an `Instance` (for writing).
template <class InstanceType>
std::optional<Resolved<InstanceType>> resolvePath(InstanceType& root, std::string_view path) {
    InstanceType* instance = &root;
    std::string spelled;
    for (;;) {
        const auto dot = path.find('.');
        const auto slot = findVariable(*instance->type, path.substr(0, dot));
        if (!slot) {
            return std::nullopt;
        }
        const auto& variable = instance->type->variables[*slot];
        spelled += variable.name;
        const bool last = dot == std::string_view::npos;
        const bool isInstance = variable.type == DataType::FunctionBlock;
        if (last) {
            if (isInstance) {
                return std::nullopt; // an instance has no single value
            }
            return Resolved<InstanceType>{instance, *slot, spelled};
        }
        if (!isInstance) {
            return std::nullopt; // "count.x": count has no members
        }
        instance = instance->children[*slot].get();
        spelled += '.';
        path.remove_prefix(dot + 1);
    }
}

void collectVariables(const Instance& instance, const std::string& prefix, std::vector<VariableInfo>& out) {
    const auto& variables = instance.type->variables;
    for (std::size_t slot = 0; slot < variables.size(); ++slot) {
        const auto& variable = variables[slot];
        if (variable.type == DataType::FunctionBlock) {
            collectVariables(*instance.children[slot], prefix + variable.name + ".", out);
        } else {
            out.push_back(VariableInfo{prefix + variable.name, variable.type});
        }
    }
}

} // namespace

std::vector<VariableInfo> Interpreter::variables() const {
    std::vector<VariableInfo> result;
    collectVariables(*root_, "", result);
    return result;
}

std::optional<VariableInfo> Interpreter::describe(std::string_view path) const {
    const auto found = resolvePath(std::as_const(*root_), path);
    if (!found) {
        return std::nullopt;
    }
    return VariableInfo{found->path, found->instance->type->variables[found->slot].type};
}

std::optional<Value> Interpreter::valueOf(std::string_view path, const ProcessImage& image) const {
    const auto found = resolvePath(std::as_const(*root_), path);
    if (!found) {
        return std::nullopt;
    }
    return load(*found->instance, found->slot, &image);
}

bool Interpreter::setValue(std::string_view path, const Value& value, ProcessImage& image) {
    const auto found = resolvePath(*root_, path);
    if (!found) {
        return false;
    }
    ImageBinding binding(image_, image); // AT variables write to the process image
    store(*found->instance, found->slot, value);
    return true;
}

Value Interpreter::valueOf(std::size_t slot, const ProcessImage& image) const { return load(*root_, slot, &image); }

// ---------------------------------------------------------------- Variables

Value Interpreter::load(const Instance& self, std::size_t slot, const ProcessImage* image) const {
    const auto& variable = self.type->variables[slot];
    if (!variable.address) {
        return self.values[slot];
    }
    // AT variables: the analyzer made sure the address size matches the type
    // (bit for BOOL, word for INT, double word for DINT/REAL/TIME).
    const auto raw = image->read(*variable.address);
    switch (variable.type) {
    case DataType::Bool: return raw != 0;
    case DataType::Int: return std::int64_t{static_cast<std::int16_t>(raw)};
    case DataType::DInt:
    case DataType::Time: return std::int64_t{static_cast<std::int32_t>(raw)};
    case DataType::Real:
        // The 32 bits in memory *are* the float: reinterpret them, don't convert the number.
        return static_cast<double>(std::bit_cast<float>(raw));
    default: return defaultValue(variable.type);
    }
}

void Interpreter::store(Instance& self, std::size_t slot, const Value& value) {
    const auto& variable = self.type->variables[slot];
    const auto converted = convert(value, variable.type);
    if (!variable.address) {
        self.values[slot] = converted;
        return;
    }
    std::uint32_t raw = 0;
    switch (variable.type) {
    case DataType::Bool: raw = std::get<bool>(converted) ? 1u : 0u; break;
    case DataType::Real: raw = std::bit_cast<std::uint32_t>(static_cast<float>(std::get<double>(converted))); break;
    default: raw = static_cast<std::uint32_t>(std::get<std::int64_t>(converted)); break; // INT, DINT, TIME
    }
    image_->write(*variable.address, raw);
}

// ---------------------------------------------------------------- Statements

Interpreter::Flow Interpreter::execute(const StatementList& list, Instance& self) {
    for (const auto& statement : list) {
        if (const auto flow = execute(statement, self); flow != Flow::Normal) {
            return flow; // EXIT or RETURN: stop this list and pass it outwards
        }
    }
    return Flow::Normal;
}

Interpreter::Flow Interpreter::execute(const Statement& statement, Instance& self) {
    return std::visit(
        Overloaded{
            [&](const Assignment& s) {
                store(self, s.slot, evaluate(*s.value, self));
                return Flow::Normal;
            },
            [&](const CallStatement& s) {
                call(s, self);
                return Flow::Normal;
            },
            [&](const IfStatement& s) {
                for (const auto& branch : s.branches) {
                    if (std::get<bool>(evaluate(*branch.condition, self))) {
                        return execute(branch.body, self);
                    }
                }
                return execute(s.elseBody, self);
            },
            [&](const CaseStatement& s) {
                const auto selector = toInteger(evaluate(*s.selector, self));
                for (const auto& branch : s.branches) {
                    for (const auto& range : branch.labels) {
                        if (range.low <= selector && selector <= range.high) {
                            return execute(branch.body, self);
                        }
                    }
                }
                return execute(s.elseBody, self);
            },
            [&](const ForStatement& s) { return executeFor(s, statement.location, self); },
            [&](const WhileStatement& s) {
                while (std::get<bool>(evaluate(*s.condition, self))) {
                    countLoopIteration(statement.location);
                    const auto flow = execute(s.body, self);
                    if (flow == Flow::Exit) break;
                    if (flow == Flow::Return) return Flow::Return;
                }
                return Flow::Normal;
            },
            [&](const RepeatStatement& s) {
                do {
                    countLoopIteration(statement.location);
                    const auto flow = execute(s.body, self);
                    if (flow == Flow::Exit) break;
                    if (flow == Flow::Return) return Flow::Return;
                } while (!std::get<bool>(evaluate(*s.condition, self)));
                return Flow::Normal;
            },
            [&](const ExitStatement&) { return Flow::Exit; },
            [&](const ReturnStatement&) { return Flow::Return; },
        },
        statement.node);
}

// t1(IN := x, PT := T#2s): copy each given input into the instance, then run
// it. Inputs that aren't given keep the value from the previous call.
void Interpreter::call(const CallStatement& s, Instance& self) {
    Instance& instance = *self.children[s.instanceSlot];
    for (const auto& argument : s.arguments) {
        store(instance, argument.slot, evaluate(*argument.value, self));
    }
    run(instance);
}

// FOR i := from TO to BY step: the bounds are evaluated once, before the
// first iteration. The loop runs while i <= to (or i >= to when stepping down).
Interpreter::Flow Interpreter::executeFor(const ForStatement& s, SourceLocation where, Instance& self) {
    const auto to = toInteger(evaluate(*s.to, self));
    const auto step = s.step ? toInteger(evaluate(*s.step, self)) : 1;
    if (step == 0) {
        throw RuntimeError{"FOR loop with BY 0 would never end", where};
    }
    const auto type = self.type->variables[s.slot].type;

    store(self, s.slot, evaluate(*s.from, self));
    for (;;) {
        const auto current = toInteger(load(self, s.slot, image_));
        if (step > 0 ? current > to : current < to) {
            return Flow::Normal;
        }
        countLoopIteration(where);
        const auto flow = execute(s.body, self);
        if (flow == Flow::Exit) return Flow::Normal;
        if (flow == Flow::Return) return Flow::Return;
        // Wraps like the PLC would: FOR i := 0 TO 32767 never ends with an INT
        // (i wraps from 32767 to -32768), which the watchdog then catches.
        store(self, s.slot, wrapInteger(add(current, step), type));
    }
}

void Interpreter::countLoopIteration(SourceLocation where) {
    if (++loopIterations_ > loopLimit_) {
        throw RuntimeError{fmt::format("watchdog: more than {} loop iterations in one cycle", loopLimit_), where};
    }
}

// ---------------------------------------------------------------- Expressions

Value Interpreter::evaluate(const Expression& expression, Instance& self) {
    return std::visit(Overloaded{
                          [](const Literal& literal) { return literal.value; },
                          [&](const VariableRef& variable) { return load(self, variable.slot, image_); },
                          [&](const MemberRef& member) {
                              return self.children[member.instanceSlot]->values[member.memberSlot];
                          },
                          [&](const UnaryExpression& unary) -> Value {
                              const auto operand = evaluate(*unary.operand, self);
                              if (unary.op == UnaryOperator::Not) {
                                  return !std::get<bool>(operand);
                              }
                              if (expression.type == DataType::Real) {
                                  return -toReal(operand);
                              }
                              return wrapInteger(subtract(0, toInteger(operand)), expression.type);
                          },
                          [&](const BinaryExpression& binary) { return evaluateBinary(binary, expression, self); },
                      },
                      expression.node);
}

Value Interpreter::evaluateBinary(const BinaryExpression& binary, const Expression& expression, Instance& self) {
    const auto left = evaluate(*binary.left, self);
    const auto right = evaluate(*binary.right, self);
    const auto type = binary.operandType;

    if (isComparison(binary.op)) {
        if (type == DataType::Bool) return compare(binary.op, std::get<bool>(left), std::get<bool>(right));
        if (type == DataType::Real) return compare(binary.op, toReal(left), toReal(right));
        return compare(binary.op, toInteger(left), toInteger(right));
    }

    switch (binary.op) {
    case BinaryOperator::And: return std::get<bool>(left) && std::get<bool>(right);
    case BinaryOperator::Or: return std::get<bool>(left) || std::get<bool>(right);
    case BinaryOperator::Xor: return std::get<bool>(left) != std::get<bool>(right);
    case BinaryOperator::Power: return roundToReal(std::pow(toReal(left), toReal(right)));
    default: break;
    }

    if (type == DataType::Real) {
        const auto a = toReal(left);
        const auto b = toReal(right);
        switch (binary.op) {
        case BinaryOperator::Add: return roundToReal(a + b);
        case BinaryOperator::Subtract: return roundToReal(a - b);
        case BinaryOperator::Multiply: return roundToReal(a * b);
        case BinaryOperator::Divide: return roundToReal(a / b); // x / 0.0 gives infinity, as on a real PLC
        default: break;
        }
    }

    // Integers, and TIME (milliseconds, also an integer underneath).
    const auto a = toInteger(left);
    const auto b = toInteger(right);
    switch (binary.op) {
    case BinaryOperator::Add: return wrapInteger(add(a, b), type);
    case BinaryOperator::Subtract: return wrapInteger(subtract(a, b), type);
    case BinaryOperator::Multiply: return wrapInteger(multiply(a, b), type);
    case BinaryOperator::Divide:
    case BinaryOperator::Modulo:
        if (b == 0) {
            throw RuntimeError{"division by zero", expression.location};
        }
        if (b == -1) { // avoids the one overflowing case, INT64_MIN / -1
            return binary.op == BinaryOperator::Divide ? wrapInteger(subtract(0, a), type) : std::int64_t{0};
        }
        // C++ rounds integer division towards zero, like IEC: -7 / 2 = -3 and -7 MOD 2 = -1.
        return wrapInteger(binary.op == BinaryOperator::Divide ? a / b : a % b, type);
    default: break;
    }
    throw RuntimeError{fmt::format("operator '{}' is not supported here", operatorSymbol(binary.op)),
                       expression.location};
}

} // namespace softplc::st
