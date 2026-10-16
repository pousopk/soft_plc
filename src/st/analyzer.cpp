#include "st/analyzer.hpp"
#include "st/builtins.hpp"
#include "st/parser.hpp"
#include "util/overloaded.hpp"
#include "util/text.hpp"
#include <algorithm>
#include <fmt/format.h>
#include <optional>
#include <set>
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
        if (const auto* value = std::get_if<std::int64_t>(&literal->value); value && !literal->isTime) {
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

// Finds a variable of a POU by name (case-insensitive).
std::optional<std::size_t> findVariable(const Pou& pou, const std::string& name) {
    const auto upper = toUpper(name);
    for (std::size_t slot = 0; slot < pou.variables.size(); ++slot) {
        if (toUpper(pou.variables[slot].name) == upper) {
            return slot;
        }
    }
    return std::nullopt;
}

class Analyzer {
public:
    explicit Analyzer(CompilationUnit& unit) : unit_(unit) {}

    void run() {
        registerFunctionBlockTypes();
        for (auto& functionBlock : unit_.functionBlocks) {
            checkPou(functionBlock);
        }
        checkPou(unit_.program);
        checkNoFunctionBlockContainsItself();
    }

private:
    [[noreturn]] static void fail(SourceLocation where, std::string message) {
        throw AnalysisError{Diagnostic{std::move(message), where}};
    }

    // ------------------------------------------------------------ Function block types

    void registerFunctionBlockTypes() {
        for (const auto& builtin : builtinFunctionBlocks()) {
            types_.emplace(toUpper(builtin.name), &builtin);
        }
        for (const auto& functionBlock : unit_.functionBlocks) {
            const auto [it, inserted] = types_.emplace(toUpper(functionBlock.name), &functionBlock);
            if (!inserted) {
                if (it->second->builtin) {
                    fail(functionBlock.location,
                         fmt::format("'{}' is a standard function block; choose another name", functionBlock.name));
                }
                fail(functionBlock.location, fmt::format("function block '{}' is already defined on line {}",
                                                         functionBlock.name, it->second->location.line));
            }
        }
    }

    // A function block may contain instances of other blocks, but never of
    // itself: an instance would contain an instance, which contains an instance...
    void checkNoFunctionBlockContainsItself() const {
        for (const auto& functionBlock : unit_.functionBlocks) {
            std::vector<const Pou*> path{&functionBlock};
            checkContents(functionBlock, path);
        }
    }

    void checkContents(const Pou& pou, std::vector<const Pou*>& path) const {
        for (const auto& variable : pou.variables) {
            const Pou* inner = variable.functionBlock;
            if (inner == nullptr || inner->builtin) {
                continue;
            }
            if (std::find(path.begin(), path.end(), inner) != path.end()) {
                std::string chain;
                for (const Pou* step : path) {
                    chain += step->name + " -> ";
                }
                fail(variable.location, fmt::format("function block '{}' contains itself: {}{}", inner->name, chain,
                                                    inner->name));
            }
            path.push_back(inner);
            checkContents(*inner, path);
            path.pop_back();
        }
    }

    // ------------------------------------------------------------ One POU

    void checkPou(Pou& pou) {
        pou_ = &pou;
        slots_.clear();
        loopVariables_.clear();
        loopDepth_ = 0;

        declareVariables();
        for (auto& variable : pou.variables) {
            if (variable.initialValue) {
                checkInitialValue(variable);
            }
        }
        checkStatements(pou.body);
    }

    bool inProgram() const { return pou_->kind == PouKind::Program; }

    void declareVariables() {
        for (std::size_t slot = 0; slot < pou_->variables.size(); ++slot) {
            auto& variable = pou_->variables[slot];
            const auto [it, inserted] = slots_.emplace(toUpper(variable.name), slot);
            if (!inserted) {
                const auto& first = pou_->variables[it->second];
                fail(variable.location,
                     fmt::format("'{}' is already declared on line {}", variable.name, first.location.line));
            }
            if (inProgram() && variable.section != VarSection::Local) {
                fail(variable.location, "a PROGRAM can only have VAR blocks; VAR_INPUT and VAR_OUTPUT are for "
                                        "function blocks");
            }
            if (variable.type == DataType::FunctionBlock) {
                resolveFunctionBlockType(variable);
            }
            if (variable.address) {
                checkAddress(variable);
            }
        }
    }

    void resolveFunctionBlockType(VariableDeclaration& variable) const {
        const auto it = types_.find(toUpper(variable.typeName));
        if (it == types_.end()) {
            fail(variable.location, fmt::format("unknown type '{}' (not BOOL, INT, DINT, REAL, TIME or a known "
                                                "function block)",
                                                variable.typeName));
        }
        variable.functionBlock = it->second;
        if (variable.section != VarSection::Local) {
            fail(variable.location, "inputs and outputs must be BOOL, INT, DINT, REAL or TIME, not function blocks");
        }
        if (variable.initialValue) {
            fail(variable.location,
                 fmt::format("'{}' is a {} instance and can't have an initial value", variable.name, it->second->name));
        }
    }

    void checkAddress(const VariableDeclaration& variable) const {
        if (!inProgram()) {
            fail(variable.location, fmt::format("AT can only be used in the PROGRAM, not inside function block '{}'",
                                                pou_->name));
        }
        const auto size = variable.address->size;
        const bool fits = (variable.type == DataType::Bool && size == AccessSize::Bit) ||
                          (variable.type == DataType::Int && size == AccessSize::Word) ||
                          ((variable.type == DataType::DInt || variable.type == DataType::Real ||
                            variable.type == DataType::Time) &&
                           size == AccessSize::DWord);
        if (!fits) {
            const char* needed = variable.type == DataType::Bool           ? "a bit address like %IX0.0"
                                 : variable.type == DataType::Int          ? "a word address like %MW0"
                                 : variable.type == DataType::FunctionBlock ? "no address at all"
                                                                            : "a double word address like %MD0";
            const auto typeText =
                variable.type == DataType::FunctionBlock ? variable.typeName : std::string(dataTypeName(variable.type));
            fail(variable.location, fmt::format("{} '{}' needs {}, not {}", typeText, variable.name, needed,
                                                variable.address->toString()));
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

    const VariableDeclaration& variableAt(std::size_t slot) const { return pou_->variables[slot]; }

    // Looks up `instance` and checks that it's a function block instance.
    std::pair<std::size_t, const Pou*> lookupInstance(const std::string& instance, SourceLocation where,
                                                      const char* whatFor) const {
        const auto slot = lookup(instance, where);
        const auto& variable = variableAt(slot);
        if (variable.type != DataType::FunctionBlock) {
            fail(where, fmt::format("'{}' is not a function block instance, so it {}", instance, whatFor));
        }
        return {slot, variable.functionBlock};
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
            const char* hint = isNumeric(source) && isNumeric(target) ? " (values might not fit)"
                               : target == DataType::Time            ? " (write times like T#500ms)"
                                                                     : "";
            fail(value.location, fmt::format("can't assign {} to '{}', which is {}{}", dataTypeName(source),
                                             targetName, dataTypeName(target), hint));
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
                                         [&](MemberRef& member) { return memberType(member, expression); },
                                         [&](UnaryExpression& unary) { return unaryType(unary, expression); },
                                         [&](BinaryExpression& binary) { return binaryType(binary, expression); },
                                     },
                                     expression.node);
        return expression.type;
    }

    static DataType literalType(const Literal& literal) {
        if (literal.isTime) return DataType::Time;
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
        const auto& declaration = variableAt(variable.slot);
        if (declaration.type == DataType::FunctionBlock) {
            fail(expression.location, fmt::format("'{}' is a {} instance, not a value; use one of its outputs, "
                                                  "like {}.Q",
                                                  variable.name, declaration.functionBlock->name, variable.name));
        }
        return declaration.type;
    }

    DataType memberType(MemberRef& member, const Expression& expression) {
        if (constantsOnly_) {
            fail(expression.location, "initial values must be constants");
        }
        const auto [instanceSlot, type] = lookupInstance(member.instance, expression.location, "has no members");
        const auto memberSlot = findVariable(*type, member.member);
        if (!memberSlot) {
            fail(expression.location, fmt::format("{} has no input or output called '{}'", type->name, member.member));
        }
        const auto& declaration = type->variables[*memberSlot];
        if (declaration.section == VarSection::Local) {
            fail(expression.location, fmt::format("'{}' is internal to {}; only its inputs and outputs can be read",
                                                  declaration.name, type->name));
        }
        member.instanceSlot = instanceSlot;
        member.memberSlot = *memberSlot;
        return declaration.type;
    }

    DataType unaryType(UnaryExpression& unary, const Expression& expression) {
        const auto operand = checkExpression(*unary.operand);
        if (unary.op == UnaryOperator::Negate) {
            if (!isNumeric(operand) && operand != DataType::Time) {
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

        if (left == DataType::Time || right == DataType::Time) {
            return timeOperation(binary, left, right, mismatch);
        }

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

    // TIME rules: TIME + TIME, TIME - TIME, TIME * integer, integer * TIME,
    // TIME / integer, and comparing two TIMEs.
    template <class Mismatch>
    static DataType timeOperation(BinaryExpression& binary, DataType left, DataType right, Mismatch mismatch) {
        const bool bothTime = left == DataType::Time && right == DataType::Time;
        binary.operandType = DataType::Time;
        switch (binary.op) {
        case BinaryOperator::Add:
        case BinaryOperator::Subtract:
            if (!bothTime) mismatch("two TIMEs");
            return DataType::Time;
        case BinaryOperator::Multiply:
            if (!(left == DataType::Time && isInteger(right)) && !(isInteger(left) && right == DataType::Time)) {
                mismatch("a TIME and an integer");
            }
            return DataType::Time;
        case BinaryOperator::Divide:
            if (!(left == DataType::Time && isInteger(right))) mismatch("a TIME divided by an integer");
            return DataType::Time;
        case BinaryOperator::Equal:
        case BinaryOperator::NotEqual:
        case BinaryOperator::Less:
        case BinaryOperator::LessEqual:
        case BinaryOperator::Greater:
        case BinaryOperator::GreaterEqual:
            if (!bothTime) mismatch("two TIMEs");
            return DataType::Bool;
        default: mismatch("operands that aren't TIME");
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
        const auto& target = variableAt(s.slot);
        if (target.type == DataType::FunctionBlock) {
            fail(where, fmt::format("can't assign to '{}': it's a {} instance", s.target, target.functionBlock->name));
        }
        if (std::find(loopVariables_.begin(), loopVariables_.end(), s.slot) != loopVariables_.end()) {
            fail(where, fmt::format("'{}' is the variable of a FOR loop and can't be changed inside it", s.target));
        }
        checkExpression(*s.value);
        checkAssignable(target.type, *s.value, s.target);
    }

    void check(CallStatement& s, SourceLocation where) {
        const auto [instanceSlot, type] = lookupInstance(s.instance, where, "can't be called");
        s.instanceSlot = instanceSlot;

        std::set<std::size_t> given;
        for (auto& argument : s.arguments) {
            const auto memberSlot = findVariable(*type, argument.name);
            if (!memberSlot) {
                fail(argument.location, fmt::format("{} has no input called '{}'", type->name, argument.name));
            }
            const auto& input = type->variables[*memberSlot];
            if (input.section == VarSection::Output) {
                fail(argument.location, fmt::format("'{}' is an output of {}: read it after the call as {}.{}",
                                                    input.name, type->name, s.instance, input.name));
            }
            if (input.section == VarSection::Local) {
                fail(argument.location, fmt::format("'{}' is internal to {}; only its inputs can be set in a call",
                                                    input.name, type->name));
            }
            if (!given.insert(*memberSlot).second) {
                fail(argument.location, fmt::format("'{}' is given twice", input.name));
            }
            checkExpression(*argument.value);
            checkAssignable(input.type, *argument.value, s.instance + "." + input.name);
            argument.slot = *memberSlot;
        }
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
        const auto& variable = variableAt(s.slot);
        if (variable.type != DataType::Int && variable.type != DataType::DInt) {
            const auto typeText =
                variable.type == DataType::FunctionBlock ? variable.typeName : std::string(dataTypeName(variable.type));
            fail(where, fmt::format("the FOR variable '{}' must be INT or DINT, not {}", s.variable, typeText));
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

    CompilationUnit& unit_;
    std::unordered_map<std::string, const Pou*> types_; // upper-case name -> function block (user or standard)

    // State for the POU being checked
    Pou* pou_ = nullptr;
    std::unordered_map<std::string, std::size_t> slots_; // upper-case name -> index in pou_->variables
    std::vector<std::size_t> loopVariables_;             // slots of the FOR variables we're currently inside
    int loopDepth_ = 0;
    bool constantsOnly_ = false; // true while checking initial values
};

} // namespace

tl::expected<void, Diagnostic> analyze(CompilationUnit& unit) {
    try {
        Analyzer(unit).run();
        return {};
    } catch (const AnalysisError& error) {
        return tl::unexpected(error.diagnostic);
    }
}

tl::expected<CompilationUnit, Diagnostic> compileProgram(std::string_view source) {
    auto unit = parseProgram(source);
    if (!unit) {
        return tl::unexpected(unit.error());
    }
    if (auto checked = analyze(*unit); !checked) {
        return tl::unexpected(checked.error());
    }
    return std::move(*unit);
}

} // namespace softplc::st
