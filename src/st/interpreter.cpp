#include "st/interpreter.hpp"
#include "util/overloaded.hpp"
#include "util/text.hpp"
#include <bit>
#include <cmath>
#include <fmt/format.h>
#include <limits>

namespace softplc::st {

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
    case DataType::DInt: return static_cast<std::int32_t>(value);
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

Interpreter::Interpreter(ProgramUnit program, std::uint64_t loopLimit)
    : program_(std::move(program)), loopLimit_(loopLimit) {
    values_.reserve(program_.variables.size());
    for (const auto& variable : program_.variables) {
        // Initial values are constants (the analyzer checked), so evaluating
        // them doesn't need a process image.
        values_.push_back(variable.initialValue ? convert(evaluate(*variable.initialValue), variable.type)
                                                : defaultValue(variable.type));
    }
}

void Interpreter::runCycle(ProcessImage& image) {
    ImageBinding binding(image_, image);
    loopIterations_ = 0;
    execute(program_.body); // a RETURN just ends the cycle early
}

std::optional<Value> Interpreter::valueOf(std::string_view name, const ProcessImage& image) const {
    const auto upper = toUpper(name);
    for (std::size_t slot = 0; slot < program_.variables.size(); ++slot) {
        if (toUpper(program_.variables[slot].name) == upper) {
            return load(slot, image);
        }
    }
    return std::nullopt;
}

Value Interpreter::valueOf(std::size_t slot, const ProcessImage& image) const { return load(slot, image); }

// ---------------------------------------------------------------- Variables

Value Interpreter::load(std::size_t slot, const ProcessImage& image) const {
    const auto& variable = program_.variables[slot];
    if (!variable.address) {
        return values_[slot];
    }
    const auto& a = *variable.address;
    switch (variable.type) {
    case DataType::Bool: return image.readBit(a.area, a.byteOffset, a.bit);
    case DataType::Int: return std::int64_t{static_cast<std::int16_t>(image.readWord(a.area, a.byteOffset))};
    case DataType::DInt: return std::int64_t{static_cast<std::int32_t>(image.readDWord(a.area, a.byteOffset))};
    case DataType::Real:
        // The 32 bits in memory *are* the float: reinterpret them, don't convert the number.
        return static_cast<double>(std::bit_cast<float>(image.readDWord(a.area, a.byteOffset)));
    default: return defaultValue(variable.type);
    }
}

void Interpreter::store(std::size_t slot, const Value& value) {
    const auto& variable = program_.variables[slot];
    const auto converted = convert(value, variable.type);
    if (!variable.address) {
        values_[slot] = converted;
        return;
    }
    const auto& a = *variable.address;
    switch (variable.type) {
    case DataType::Bool: image_->writeBit(a.area, a.byteOffset, a.bit, std::get<bool>(converted)); break;
    case DataType::Int:
        image_->writeWord(a.area, a.byteOffset, static_cast<std::uint16_t>(std::get<std::int64_t>(converted)));
        break;
    case DataType::DInt:
        image_->writeDWord(a.area, a.byteOffset, static_cast<std::uint32_t>(std::get<std::int64_t>(converted)));
        break;
    case DataType::Real:
        image_->writeDWord(a.area, a.byteOffset,
                           std::bit_cast<std::uint32_t>(static_cast<float>(std::get<double>(converted))));
        break;
    default: break;
    }
}

// ---------------------------------------------------------------- Statements

Interpreter::Flow Interpreter::execute(const StatementList& list) {
    for (const auto& statement : list) {
        if (const auto flow = execute(statement); flow != Flow::Normal) {
            return flow; // EXIT or RETURN: stop this list and pass it outwards
        }
    }
    return Flow::Normal;
}

Interpreter::Flow Interpreter::execute(const Statement& statement) {
    return std::visit(
        Overloaded{
            [&](const Assignment& s) {
                store(s.slot, evaluate(*s.value));
                return Flow::Normal;
            },
            [&](const IfStatement& s) {
                for (const auto& branch : s.branches) {
                    if (std::get<bool>(evaluate(*branch.condition))) {
                        return execute(branch.body);
                    }
                }
                return execute(s.elseBody);
            },
            [&](const CaseStatement& s) {
                const auto selector = toInteger(evaluate(*s.selector));
                for (const auto& branch : s.branches) {
                    for (const auto& range : branch.labels) {
                        if (range.low <= selector && selector <= range.high) {
                            return execute(branch.body);
                        }
                    }
                }
                return execute(s.elseBody);
            },
            [&](const ForStatement& s) { return executeFor(s, statement.location); },
            [&](const WhileStatement& s) {
                while (std::get<bool>(evaluate(*s.condition))) {
                    countLoopIteration(statement.location);
                    const auto flow = execute(s.body);
                    if (flow == Flow::Exit) break;
                    if (flow == Flow::Return) return Flow::Return;
                }
                return Flow::Normal;
            },
            [&](const RepeatStatement& s) {
                do {
                    countLoopIteration(statement.location);
                    const auto flow = execute(s.body);
                    if (flow == Flow::Exit) break;
                    if (flow == Flow::Return) return Flow::Return;
                } while (!std::get<bool>(evaluate(*s.condition)));
                return Flow::Normal;
            },
            [&](const ExitStatement&) { return Flow::Exit; },
            [&](const ReturnStatement&) { return Flow::Return; },
        },
        statement.node);
}

// FOR i := from TO to BY step: the bounds are evaluated once, before the
// first iteration. The loop runs while i <= to (or i >= to when stepping down).
Interpreter::Flow Interpreter::executeFor(const ForStatement& s, SourceLocation where) {
    const auto to = toInteger(evaluate(*s.to));
    const auto step = s.step ? toInteger(evaluate(*s.step)) : 1;
    if (step == 0) {
        throw RuntimeError{"FOR loop with BY 0 would never end", where};
    }
    const auto type = program_.variables[s.slot].type;

    store(s.slot, evaluate(*s.from));
    for (;;) {
        const auto current = toInteger(load(s.slot, *image_));
        if (step > 0 ? current > to : current < to) {
            return Flow::Normal;
        }
        countLoopIteration(where);
        const auto flow = execute(s.body);
        if (flow == Flow::Exit) return Flow::Normal;
        if (flow == Flow::Return) return Flow::Return;
        // Wraps like the PLC would: FOR i := 0 TO 32767 never ends with an INT
        // (i wraps from 32767 to -32768), which the watchdog then catches.
        store(s.slot, wrapInteger(add(current, step), type));
    }
}

void Interpreter::countLoopIteration(SourceLocation where) {
    if (++loopIterations_ > loopLimit_) {
        throw RuntimeError{fmt::format("watchdog: more than {} loop iterations in one cycle", loopLimit_), where};
    }
}

// ---------------------------------------------------------------- Expressions

Value Interpreter::evaluate(const Expression& expression) {
    return std::visit(Overloaded{
                          [](const Literal& literal) { return literal.value; },
                          [&](const VariableRef& variable) { return load(variable.slot, *image_); },
                          [&](const UnaryExpression& unary) -> Value {
                              const auto operand = evaluate(*unary.operand);
                              if (unary.op == UnaryOperator::Not) {
                                  return !std::get<bool>(operand);
                              }
                              if (expression.type == DataType::Real) {
                                  return -toReal(operand);
                              }
                              return wrapInteger(subtract(0, toInteger(operand)), expression.type);
                          },
                          [&](const BinaryExpression& binary) { return evaluateBinary(binary, expression); },
                      },
                      expression.node);
}

Value Interpreter::evaluateBinary(const BinaryExpression& binary, const Expression& expression) {
    const auto left = evaluate(*binary.left);
    const auto right = evaluate(*binary.right);
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
