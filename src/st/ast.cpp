#include "st/ast.hpp"
#include "util/overloaded.hpp"
#include <fmt/format.h>

namespace softplc::st {

namespace {

// Always shows a decimal point, so 3.0 prints as "3.0" and not "3".
std::string formatReal(double value) {
    auto text = fmt::format("{}", value);
    if (text.find_first_of(".eEn") == std::string::npos) { // 'n' covers inf and nan
        text += ".0";
    }
    return text;
}

// Appends lines to a string, indented two spaces per level.
class TreeWriter {
public:
    explicit TreeWriter(std::string& out) : out_(out) {}

    void line(int depth, const std::string& text) {
        out_.append(static_cast<std::size_t>(depth) * 2, ' ');
        out_ += text;
        out_ += '\n';
    }

    void expression(const Expression& expression, int depth) {
        std::visit(Overloaded{
                       [&](const Literal& literal) {
                           const char* kind = std::holds_alternative<bool>(literal.value)           ? "Bool"
                                              : std::holds_alternative<std::int64_t>(literal.value) ? "Integer"
                                                                                                    : "Real";
                           line(depth, fmt::format("{} {}", kind, formatValue(literal.value)));
                       },
                       [&](const VariableRef& variable) { line(depth, fmt::format("Variable {}", variable.name)); },
                       [&](const UnaryExpression& unary) {
                           line(depth, fmt::format("Unary {}", operatorSymbol(unary.op)));
                           this->expression(*unary.operand, depth + 1);
                       },
                       [&](const BinaryExpression& binary) {
                           line(depth, fmt::format("Binary {}", operatorSymbol(binary.op)));
                           this->expression(*binary.left, depth + 1);
                           this->expression(*binary.right, depth + 1);
                       },
                   },
                   expression.node);
    }

    void statements(const StatementList& list, int depth) {
        for (const auto& statement : list) {
            this->statement(statement, depth);
        }
    }

    // A labelled child: the label on one line, its content indented below.
    void labelled(int depth, const char* label, const Expression& child) {
        line(depth, label);
        expression(child, depth + 1);
    }

    void labelled(int depth, const char* label, const StatementList& body) {
        line(depth, label);
        statements(body, depth + 1);
    }

    void statement(const Statement& statement, int depth) {
        std::visit(Overloaded{
                       [&](const Assignment& s) {
                           line(depth, fmt::format("Assignment {}", s.target));
                           expression(*s.value, depth + 1);
                       },
                       [&](const IfStatement& s) {
                           line(depth, "If");
                           for (std::size_t i = 0; i < s.branches.size(); ++i) {
                               labelled(depth + 1, i == 0 ? "Condition" : "ElsifCondition", *s.branches[i].condition);
                               labelled(depth + 1, "Then", s.branches[i].body);
                           }
                           if (!s.elseBody.empty()) {
                               labelled(depth + 1, "Else", s.elseBody);
                           }
                       },
                       [&](const CaseStatement& s) {
                           line(depth, "Case");
                           labelled(depth + 1, "Selector", *s.selector);
                           for (const auto& branch : s.branches) {
                               std::string labels;
                               for (const auto& range : branch.labels) {
                                   if (!labels.empty()) {
                                       labels += ", ";
                                   }
                                   labels += range.low == range.high ? fmt::format("{}", range.low)
                                                                     : fmt::format("{}..{}", range.low, range.high);
                               }
                               labelled(depth + 1, fmt::format("Labels {}", labels).c_str(), branch.body);
                           }
                           if (!s.elseBody.empty()) {
                               labelled(depth + 1, "Else", s.elseBody);
                           }
                       },
                       [&](const ForStatement& s) {
                           line(depth, fmt::format("For {}", s.variable));
                           labelled(depth + 1, "From", *s.from);
                           labelled(depth + 1, "To", *s.to);
                           if (s.step) {
                               labelled(depth + 1, "By", *s.step);
                           }
                           labelled(depth + 1, "Do", s.body);
                       },
                       [&](const WhileStatement& s) {
                           line(depth, "While");
                           labelled(depth + 1, "Condition", *s.condition);
                           labelled(depth + 1, "Do", s.body);
                       },
                       [&](const RepeatStatement& s) {
                           line(depth, "Repeat");
                           labelled(depth + 1, "Do", s.body);
                           labelled(depth + 1, "Until", *s.condition);
                       },
                       [&](const ExitStatement&) { line(depth, "Exit"); },
                       [&](const ReturnStatement&) { line(depth, "Return"); },
                   },
                   statement.node);
    }

private:
    std::string& out_;
};

} // namespace

const char* dataTypeName(DataType type) {
    switch (type) {
    case DataType::Unknown: return "unknown";
    case DataType::Bool: return "BOOL";
    case DataType::Int: return "INT";
    case DataType::DInt: return "DINT";
    case DataType::Real: return "REAL";
    case DataType::AnyInt: return "integer literal";
    }
    return "unknown";
}

std::string formatValue(const Value& value) {
    return std::visit(Overloaded{
                          [](bool b) { return std::string(b ? "TRUE" : "FALSE"); },
                          [](std::int64_t i) { return fmt::format("{}", i); },
                          [](double d) { return formatReal(d); },
                      },
                      value);
}

const char* operatorSymbol(UnaryOperator op) {
    switch (op) {
    case UnaryOperator::Negate: return "-";
    case UnaryOperator::Not: return "NOT";
    }
    return "?";
}

const char* operatorSymbol(BinaryOperator op) {
    switch (op) {
    case BinaryOperator::Add: return "+";
    case BinaryOperator::Subtract: return "-";
    case BinaryOperator::Multiply: return "*";
    case BinaryOperator::Divide: return "/";
    case BinaryOperator::Modulo: return "MOD";
    case BinaryOperator::Power: return "**";
    case BinaryOperator::Equal: return "=";
    case BinaryOperator::NotEqual: return "<>";
    case BinaryOperator::Less: return "<";
    case BinaryOperator::LessEqual: return "<=";
    case BinaryOperator::Greater: return ">";
    case BinaryOperator::GreaterEqual: return ">=";
    case BinaryOperator::And: return "AND";
    case BinaryOperator::Or: return "OR";
    case BinaryOperator::Xor: return "XOR";
    }
    return "?";
}

ExpressionPtr cloneExpression(const Expression& expression) {
    auto node = std::visit(
        Overloaded{
            [](const Literal& literal) -> decltype(Expression::node) { return literal; },
            [](const VariableRef& variable) -> decltype(Expression::node) { return variable; },
            [](const UnaryExpression& unary) -> decltype(Expression::node) {
                return UnaryExpression{unary.op, cloneExpression(*unary.operand)};
            },
            [](const BinaryExpression& binary) -> decltype(Expression::node) {
                return BinaryExpression{binary.op, cloneExpression(*binary.left), cloneExpression(*binary.right),
                                        binary.operandType};
            },
        },
        expression.node);
    return std::make_unique<Expression>(Expression{std::move(node), expression.location, expression.type});
}

std::string formatExpression(const Expression& expression) {
    return std::visit(Overloaded{
                          [](const Literal& literal) { return formatValue(literal.value); },
                          [](const VariableRef& variable) { return variable.name; },
                          [](const UnaryExpression& unary) {
                              const char* separator = unary.op == UnaryOperator::Not ? " " : "";
                              return fmt::format("({}{}{})", operatorSymbol(unary.op), separator,
                                                 formatExpression(*unary.operand));
                          },
                          [](const BinaryExpression& binary) {
                              return fmt::format("({} {} {})", formatExpression(*binary.left),
                                                 operatorSymbol(binary.op), formatExpression(*binary.right));
                          },
                      },
                      expression.node);
}

std::string dumpTree(const Statement& statement) {
    std::string out;
    TreeWriter(out).statement(statement, 0);
    return out;
}

std::string dumpTree(const ProgramUnit& program) {
    std::string out;
    TreeWriter writer(out);
    writer.line(0, fmt::format("Program {}", program.name));
    writer.line(1, "Variables");
    for (const auto& variable : program.variables) {
        std::string text = variable.name;
        if (variable.address) {
            text += " AT " + variable.address->toString();
        }
        text += fmt::format(" : {}", dataTypeName(variable.type));
        if (variable.initialValue) {
            text += " := " + formatExpression(*variable.initialValue);
        }
        writer.line(2, text);
    }
    writer.labelled(1, "Body", program.body);
    return out;
}

} // namespace softplc::st
