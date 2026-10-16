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
    return std::visit(Overloaded{
                          [](bool b) { return std::string(b ? "TRUE" : "FALSE"); },
                          [](std::int64_t i) { return fmt::format("{}", i); },
                          [](double d) { return formatReal(d); },
                      },
                      literal.value);
}

void dumpExpression(const Expression& expression, int depth, std::string& out) {
    const std::string indent(static_cast<std::size_t>(depth) * 2, ' ');
    std::visit(Overloaded{
                   [&](const Literal& literal) {
                       const char* kind = std::holds_alternative<bool>(literal.value)           ? "Bool"
                                          : std::holds_alternative<std::int64_t>(literal.value) ? "Integer"
                                                                                                : "Real";
                       out += fmt::format("{}{} {}\n", indent, kind, formatLiteral(literal));
                   },
                   [&](const VariableRef& variable) { out += fmt::format("{}Variable {}\n", indent, variable.name); },
                   [&](const UnaryExpression& unary) {
                       out += fmt::format("{}Unary {}\n", indent, operatorSymbol(unary.op));
                       dumpExpression(*unary.operand, depth + 1, out);
                   },
                   [&](const BinaryExpression& binary) {
                       out += fmt::format("{}Binary {}\n", indent, operatorSymbol(binary.op));
                       dumpExpression(*binary.left, depth + 1, out);
                       dumpExpression(*binary.right, depth + 1, out);
                   },
               },
               expression.node);
}

} // namespace

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

std::string formatExpression(const Expression& expression) {
    return std::visit(Overloaded{
                          [](const Literal& literal) { return formatLiteral(literal); },
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

std::string dumpTree(const Assignment& assignment) {
    std::string out = fmt::format("Assignment {}\n", assignment.target);
    dumpExpression(*assignment.value, 1, out);
    return out;
}

} // namespace softplc::st
