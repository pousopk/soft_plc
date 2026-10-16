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

std::string formatLiteral(const Literal& literal) {
    return literal.isTime ? formatTime(std::get<std::int64_t>(literal.value)) : formatValue(literal.value);
}

const char* sectionName(VarSection section) {
    switch (section) {
    case VarSection::Local: return "Var";
    case VarSection::Input: return "VarInput";
    case VarSection::Output: return "VarOutput";
    }
    return "Var";
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
                           const char* kind = literal.isTime                                    ? "Time"
                                              : std::holds_alternative<bool>(literal.value)     ? "Bool"
                                              : std::holds_alternative<double>(literal.value)   ? "Real"
                                                                                                : "Integer";
                           line(depth, fmt::format("{} {}", kind, formatLiteral(literal)));
                       },
                       [&](const VariableRef& variable) { line(depth, fmt::format("Variable {}", variable.name)); },
                       [&](const MemberRef& member) {
                           line(depth, fmt::format("Member {}.{}", member.instance, member.member));
                       },
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
    void labelled(int depth, const std::string& label, const Expression& child) {
        line(depth, label);
        expression(child, depth + 1);
    }

    void labelled(int depth, const std::string& label, const StatementList& body) {
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
                               labelled(depth + 1, "Labels " + labels, branch.body);
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
                       [&](const CallStatement& s) {
                           line(depth, fmt::format("Call {}", s.instance));
                           for (const auto& argument : s.arguments) {
                               labelled(depth + 1, "Argument " + argument.name, *argument.value);
                           }
                       },
                   },
                   statement.node);
    }

    void pou(const Pou& pou) {
        line(0, fmt::format("{} {}", pou.kind == PouKind::Program ? "Program" : "FunctionBlock", pou.name));
        for (const auto section : {VarSection::Input, VarSection::Output, VarSection::Local}) {
            bool headerWritten = false;
            for (const auto& variable : pou.variables) {
                if (variable.section != section) {
                    continue;
                }
                if (!headerWritten) {
                    line(1, sectionName(section));
                    headerWritten = true;
                }
                line(2, declaration(variable));
            }
        }
        labelled(1, "Body", pou.body);
    }

private:
    static std::string declaration(const VariableDeclaration& variable) {
        std::string text = variable.name;
        if (variable.address) {
            text += " AT " + variable.address->toString();
        }
        text += " : ";
        text += variable.type == DataType::FunctionBlock ? variable.typeName : dataTypeName(variable.type);
        if (variable.initialValue) {
            text += " := " + formatExpression(*variable.initialValue);
        }
        return text;
    }

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
    case DataType::Time: return "TIME";
    case DataType::AnyInt: return "integer literal";
    case DataType::FunctionBlock: return "function block";
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

std::string formatValue(const Value& value, DataType type) {
    if (type == DataType::Time) {
        return formatTime(std::get<std::int64_t>(value));
    }
    return formatValue(value);
}

std::string formatTime(std::int64_t milliseconds) {
    if (milliseconds == 0) {
        return "T#0ms";
    }
    std::string text = milliseconds < 0 ? "T#-" : "T#";
    auto rest = milliseconds < 0 ? -milliseconds : milliseconds;
    constexpr std::pair<std::int64_t, const char*> units[] = {
        {86'400'000, "d"}, {3'600'000, "h"}, {60'000, "m"}, {1'000, "s"}, {1, "ms"},
    };
    for (const auto& [size, unit] : units) {
        if (rest >= size) {
            text += fmt::format("{}{}", rest / size, unit);
            rest %= size;
        }
    }
    return text;
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
    using Node = decltype(Expression::node);
    auto node = std::visit(Overloaded{
                               [](const Literal& literal) -> Node { return literal; },
                               [](const VariableRef& variable) -> Node { return variable; },
                               [](const MemberRef& member) -> Node { return member; },
                               [](const UnaryExpression& unary) -> Node {
                                   return UnaryExpression{unary.op, cloneExpression(*unary.operand)};
                               },
                               [](const BinaryExpression& binary) -> Node {
                                   return BinaryExpression{binary.op, cloneExpression(*binary.left),
                                                           cloneExpression(*binary.right), binary.operandType};
                               },
                           },
                           expression.node);
    return std::make_unique<Expression>(Expression{std::move(node), expression.location, expression.type});
}

std::string formatExpression(const Expression& expression) {
    return std::visit(Overloaded{
                          [](const Literal& literal) { return formatLiteral(literal); },
                          [](const VariableRef& variable) { return variable.name; },
                          [](const MemberRef& member) { return member.instance + "." + member.member; },
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

std::string dumpTree(const Pou& pou) {
    std::string out;
    TreeWriter(out).pou(pou);
    return out;
}

std::string dumpTree(const CompilationUnit& unit) {
    std::string out;
    for (const auto& functionBlock : unit.functionBlocks) {
        out += dumpTree(functionBlock);
        out += '\n';
    }
    out += dumpTree(unit.program);
    return out;
}

} // namespace softplc::st
