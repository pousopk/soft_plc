#include "st/analyzer.hpp"
#include "st/parser.hpp"
#include "util/overloaded.hpp"
#include "util/text.hpp"
#include <algorithm>
#include <fmt/format.h>
#include <optional>
#include <unordered_map>
#include <vector>

namespace softplc::st {

namespace {

// Thrown inside the analyzer to stop at the first error; analyze() catches it.
struct AnalysisError {
    Diagnostic diagnostic;
};

bool isInteger(DataType type) { return type == DataType::Int || type == DataType::DInt || type == DataType::AnyInt; }
bool isNumeric(DataType type) { return isInteger(type) || type == DataType::Real; }

// The type two numbers are brought to before an operation: the "widest" one.
// REAL beats DINT, DINT beats INT, and a literal adapts to the other side.
DataType commonNumericType(DataType a, DataType b) {
    if (a == DataType::Real || b == DataType::Real) return DataType::Real;
    if (a == DataType::DInt || b == DataType::DInt) return DataType::DInt;
    if (a == DataType::Int || b == DataType::Int) return DataType::Int;
    return DataType::AnyInt;
}

// The value of an integer literal, also when written with a minus: 5 or -5.
std::optional<std::int64_t> integerConstant(const Expression& expression) {
    if (const auto* literal = std::get_if<Literal>(&expression.node)) {
        if (const auto* value = std::get_if<std::int64_t>(&literal->value)) {
            return *value;
        }
    }
    if (const auto* unary = std::get_if<UnaryExpression>(&expression.node)) {
        if (unary->op == UnaryOperator::Negate) {
            if (auto inner = integerConstant(*unary->operand)) {
                return -*inner;
            }
        }
    }
    return std::nullopt;
}

class Analyzer {
public:
    explicit Analyzer(ProgramUnit& program) : program_(program) {}

    void run() {
        declareVariables();
        for (auto& variable : program_.variables) {
            if (variable.initialValue) {
                checkInitialValue(variable);
            }
        }
        checkStatements(program_.body);
    }

private:
    [[noreturn]] static void fail(SourceLocation where, std::string message) {
        throw AnalysisError{Diagnostic{std::move(message), where}};
    }

    // ------------------------------------------------------------ Variables

    void declareVariables() {
        for (std::size_t slot = 0; slot < program_.variables.size(); ++slot) {
            const auto& variable = program_.variables[slot];
            const auto [it, inserted] = slots_.emplace(toUpper(variable.name), slot);
            if (!inserted) {
                const auto& first = program_.variables[it->second];
                fail(variable.location,
                     fmt::format("'{}' is already declared on line {}", variable.name, first.location.line));
            }
            if (variable.address) {
                checkAddress(variable);
            }
        }
    }

    void checkAddress(const VariableDeclaration& variable) {
        const auto size = variable.address->size;
        const bool fits = (variable.type == DataType::Bool && size == AccessSize::Bit) ||
                          (variable.type == DataType::Int && size == AccessSize::Word) ||
                          (variable.type == DataType::DInt && size == AccessSize::DWord) ||
                          (variable.type == DataType::Real && size == AccessSize::DWord);
        if (!fits) {
            const char* needed = variable.type == DataType::Bool  ? "a bit address like %IX0.0"
                                 : variable.type == DataType::Int ? "a word address like %MW0"
                                                                  : "a double word address like %MD0";
            fail(variable.location, fmt::format("{} '{}' needs {}, not {}", dataTypeName(variable.type), variable.name,
                                                needed, variable.address->toString()));
        }
        if (variable.initialValue) {
            fail(variable.location, fmt::format("'{}' can't have an initial value: its value comes from {}",
                                                variable.name, variable.address->toString()));
        }
    }

    void checkInitialValue(VariableDeclaration& variable) {
        constantsOnly_ = true;
        checkExpression(*variable.initialValue);
        constantsOnly_ = false;
        checkAssignable(variable.type, *variable.initialValue, variable.name);
    }

    std::size_t lookup(const std::string& name, SourceLocation where) const {
        const auto it = slots_.find(toUpper(name));
        if (it == slots_.end()) {
            fail(where, fmt::format("'{}' is not declared (add it to a VAR block)", name));
        }
        return it->second;
    }

    // Can a value of `value`'s type be stored in a variable of type `target`
    // without losing anything?
    void checkAssignable(DataType target, const Expression& value, const std::string& targetName) const {
        const auto source = value.type;
        if (source == target) {
            return;
        }
        if (source == DataType::AnyInt && (target == DataType::Int || target == DataType::DInt)) {
            checkLiteralFits(target, value);
            return;
        }
        const bool widening = (target == DataType::DInt && source == DataType::Int) ||
                              (target == DataType::Real && isNumeric(source));
        if (!widening) {
            fail(value.location, fmt::format("can't assign {} to '{}', which is {}{}", dataTypeName(source),
                                             targetName, dataTypeName(target),
                                             isNumeric(source) && isNumeric(target) ? " (values might not fit)" : ""));
        }
    }

    void checkLiteralFits(DataType target, const Expression& value) const {
        const auto constant = integerConstant(value);
        if (!constant) {
            return; // a calculation of literals: it wraps around at run time if too big
        }
        const auto [low, high] = target == DataType::Int ? std::pair<std::int64_t, std::int64_t>{-32768, 32767}
                                                         : std::pair<std::int64_t, std::int64_t>{-2147483648, 2147483647};
        if (*constant < low || *constant > high) {
            fail(value.location,
                 fmt::format("{} doesn't fit in {} ({} to {})", *constant, dataTypeName(target), low, high));
        }
    }

    // ------------------------------------------------------------ Expressions

    // Works out and records the type of an expression and all its children.
    DataType checkExpression(Expression& expression) {
        expression.type = std::visit(Overloaded{
                                         [&](Literal& literal) { return literalType(literal); },
                                         [&](VariableRef& variable) { return variableType(variable, expression); },
                                         [&](UnaryExpression& unary) { return unaryType(unary, expression); },
                                         [&](BinaryExpression& binary) { return binaryType(binary, expression); },
                                     },
                                     expression.node);
        return expression.type;
    }

    static DataType literalType(const Literal& literal) {
        if (std::holds_alternative<bool>(literal.value)) return DataType::Bool;
        if (std::holds_alternative<std::int64_t>(literal.value)) return DataType::AnyInt;
        return DataType::Real;
    }

    DataType variableType(VariableRef& variable, const Expression& expression) {
        if (constantsOnly_) {
            fail(expression.location, fmt::format("initial values must be constants, not variables like '{}'",
                                                  variable.name));
        }
        variable.slot = lookup(variable.name, expression.location);
        return program_.variables[variable.slot].type;
    }

    DataType unaryType(UnaryExpression& unary, const Expression& expression) {
        const auto operand = checkExpression(*unary.operand);
        if (unary.op == UnaryOperator::Negate) {
            if (!isNumeric(operand)) {
                fail(expression.location, fmt::format("'-' needs a number, not {}", dataTypeName(operand)));
            }
            return operand;
        }
        if (operand != DataType::Bool) {
            fail(expression.location, fmt::format("NOT needs a BOOL, not {}", dataTypeName(operand)));
        }
        return DataType::Bool;
    }

    DataType binaryType(BinaryExpression& binary, const Expression& expression) {
        const auto left = checkExpression(*binary.left);
        const auto right = checkExpression(*binary.right);
        const auto symbol = operatorSymbol(binary.op);
        const auto mismatch = [&](const char* needs) {
            fail(expression.location, fmt::format("'{}' needs {}, but got {} and {}", symbol, needs,
                                                  dataTypeName(left), dataTypeName(right)));
        };

        switch (binary.op) {
        case BinaryOperator::Add:
        case BinaryOperator::Subtract:
        case BinaryOperator::Multiply:
        case BinaryOperator::Divide:
            if (!isNumeric(left) || !isNumeric(right)) mismatch("numbers");
            binary.operandType = commonNumericType(left, right);
            return binary.operandType;

        case BinaryOperator::Modulo:
            if (!isInteger(left) || !isInteger(right)) mismatch("integers");
            binary.operandType = commonNumericType(left, right);
            return binary.operandType;

        case BinaryOperator::Power:
            if (!isNumeric(left) || !isNumeric(right)) mismatch("numbers");
            binary.operandType = DataType::Real;
            return DataType::Real; // like EXPT: always a REAL result

        case BinaryOperator::Less:
        case BinaryOperator::LessEqual:
        case BinaryOperator::Greater:
        case BinaryOperator::GreaterEqual:
            if (!isNumeric(left) || !isNumeric(right)) mismatch("numbers");
            binary.operandType = commonNumericType(left, right);
            return DataType::Bool;

        case BinaryOperator::Equal:
        case BinaryOperator::NotEqual:
            if (left == DataType::Bool && right == DataType::Bool) {
                binary.operandType = DataType::Bool;
            } else if (isNumeric(left) && isNumeric(right)) {
                binary.operandType = commonNumericType(left, right);
            } else {
                mismatch("two BOOLs or two numbers");
            }
            return DataType::Bool;

        case BinaryOperator::And:
        case BinaryOperator::Or:
        case BinaryOperator::Xor:
            if (left != DataType::Bool || right != DataType::Bool) mismatch("BOOLs");
            binary.operandType = DataType::Bool;
            return DataType::Bool;
        }
        return DataType::Unknown;
    }

    void requireBool(Expression& condition, const char* what) {
        const auto type = checkExpression(condition);
        if (type != DataType::Bool) {
            fail(condition.location, fmt::format("{} must be BOOL, not {}", what, dataTypeName(type)));
        }
    }

    void requireInteger(Expression& expression, const char* what) {
        const auto type = checkExpression(expression);
        if (!isInteger(type)) {
            fail(expression.location, fmt::format("{} must be an integer, not {}", what, dataTypeName(type)));
        }
    }

    // ------------------------------------------------------------ Statements

    void checkStatements(StatementList& list) {
        for (auto& statement : list) {
            std::visit([&](auto& node) { check(node, statement.location); }, statement.node);
        }
    }

    void check(Assignment& s, SourceLocation where) {
        s.slot = lookup(s.target, where);
        if (std::find(loopVariables_.begin(), loopVariables_.end(), s.slot) != loopVariables_.end()) {
            fail(where, fmt::format("'{}' is the variable of a FOR loop and can't be changed inside it", s.target));
        }
        checkExpression(*s.value);
        checkAssignable(program_.variables[s.slot].type, *s.value, s.target);
    }

    void check(IfStatement& s, SourceLocation) {
        for (auto& branch : s.branches) {
            requireBool(*branch.condition, "the IF condition");
            checkStatements(branch.body);
        }
        checkStatements(s.elseBody);
    }

    void check(CaseStatement& s, SourceLocation where) {
        requireInteger(*s.selector, "the CASE selector");
        std::vector<CaseRange> seen;
        for (auto& branch : s.branches) {
            for (const auto& range : branch.labels) {
                for (const auto& other : seen) {
                    if (range.low <= other.high && other.low <= range.high) {
                        fail(where, fmt::format("case label {} is used more than once",
                                                std::max(range.low, other.low)));
                    }
                }
                seen.push_back(range);
            }
            checkStatements(branch.body);
        }
        checkStatements(s.elseBody);
    }

    void check(ForStatement& s, SourceLocation where) {
        s.slot = lookup(s.variable, where);
        const auto& variable = program_.variables[s.slot];
        if (variable.type != DataType::Int && variable.type != DataType::DInt) {
            fail(where, fmt::format("the FOR variable '{}' must be INT or DINT, not {}", s.variable,
                                    dataTypeName(variable.type)));
        }
        for (Expression* bound : {s.from.get(), s.to.get(), s.step.get()}) {
            if (bound != nullptr) {
                requireInteger(*bound, "a FOR loop bound");
                checkAssignable(variable.type, *bound, s.variable);
            }
        }
        loopVariables_.push_back(s.slot);
        ++loopDepth_;
        checkStatements(s.body);
        --loopDepth_;
        loopVariables_.pop_back();
    }

    void check(WhileStatement& s, SourceLocation) {
        requireBool(*s.condition, "the WHILE condition");
        ++loopDepth_;
        checkStatements(s.body);
        --loopDepth_;
    }

    void check(RepeatStatement& s, SourceLocation) {
        ++loopDepth_;
        checkStatements(s.body);
        --loopDepth_;
        requireBool(*s.condition, "the UNTIL condition");
    }

    void check(ExitStatement&, SourceLocation where) const {
        if (loopDepth_ == 0) {
            fail(where, "EXIT can only be used inside FOR, WHILE or REPEAT");
        }
    }

    void check(ReturnStatement&, SourceLocation) const {}

    ProgramUnit& program_;
    std::unordered_map<std::string, std::size_t> slots_; // upper-case name -> index in program_.variables
    std::vector<std::size_t> loopVariables_;             // slots of the FOR variables we're currently inside
    int loopDepth_ = 0;
    bool constantsOnly_ = false; // true while checking initial values
};

} // namespace

tl::expected<void, Diagnostic> analyze(ProgramUnit& program) {
    try {
        Analyzer(program).run();
        return {};
    } catch (const AnalysisError& error) {
        return tl::unexpected(error.diagnostic);
    }
}

tl::expected<ProgramUnit, Diagnostic> compileProgram(std::string_view source) {
    auto program = parseProgram(source);
    if (!program) {
        return tl::unexpected(program.error());
    }
    if (auto checked = analyze(*program); !checked) {
        return tl::unexpected(checked.error());
    }
    return std::move(*program);
}

} // namespace softplc::st
