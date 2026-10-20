#include "st/ast.hpp"
#include "util/overloaded.hpp"
#include "util/text.hpp"
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
                       [&](const Access& access) {
                           line(depth, fmt::format("{} {}", access.selectors.empty() ? "Variable" : "Access",
                                                   formatAccess(access)));
                       },
                       [&](const CallExpression& call) {
                           line(depth, fmt::format("Call {}", call.name));
                           for (const auto& argument : call.arguments) {
                               labelled(depth + 1, argument.name.empty() ? "Argument" : "Argument " + argument.name,
                                        *argument.value);
                           }
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
                           line(depth, fmt::format("Assignment {}", formatAccess(s.target)));
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
        if (pou.kind == PouKind::Function) {
            line(0, fmt::format("Function {} : {}", pou.name, formatTypeSpec(pou.returnType)));
        } else {
            line(0, fmt::format("{} {}", pou.kind == PouKind::Program ? "Program" : "FunctionBlock", pou.name));
        }
        for (const auto section : {VarSection::Input, VarSection::Output, VarSection::Local}) {
            bool headerWritten = false;
            for (std::size_t slot = 0; slot < pou.variables.size(); ++slot) {
                const auto& variable = pou.variables[slot];
                if (variable.section != section || slot == pou.returnSlot) {
                    continue; // (a function's result variable is shown in the header)
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

    void typeDeclaration(const TypeDeclaration& type) {
        if (!type.isStruct) {
            line(0, fmt::format("Type {} : {}", type.name, formatTypeSpec(type.alias)));
            return;
        }
        line(0, fmt::format("Type {} : Struct", type.name));
        for (const auto& field : type.fields) {
            line(1, declaration(field));
        }
    }

private:
    static std::string declaration(const VariableDeclaration& variable) {
        std::string text = variable.name;
        if (variable.address) {
            text += " AT " + variable.address->toString();
        }
        text += " : " + formatTypeSpec(variable.typeSpec);
        if (variable.initialValue) {
            text += " := " + formatInitializer(*variable.initialValue);
        }
        return text;
    }

    std::string& out_;
};

// Every elementary type, in the order lists of them are shown.
constexpr DataType kElementaryTypes[] = {
    DataType::Bool, DataType::SInt,  DataType::USInt, DataType::Int,  DataType::UInt, DataType::DInt,
    DataType::UDInt, DataType::LInt, DataType::Byte,  DataType::Word, DataType::DWord, DataType::Real,
    DataType::Time,
};

} // namespace

const char* dataTypeName(DataType type) {
    switch (type) {
    case DataType::Unknown: return "unknown";
    case DataType::Bool: return "BOOL";
    case DataType::Int: return "INT";
    case DataType::DInt: return "DINT";
    case DataType::SInt: return "SINT";
    case DataType::USInt: return "USINT";
    case DataType::UInt: return "UINT";
    case DataType::UDInt: return "UDINT";
    case DataType::LInt: return "LINT";
    case DataType::Byte: return "BYTE";
    case DataType::Word: return "WORD";
    case DataType::DWord: return "DWORD";
    case DataType::Real: return "REAL";
    case DataType::Time: return "TIME";
    case DataType::AnyInt: return "integer literal";
    case DataType::FunctionBlock: return "function block";
    case DataType::Array: return "ARRAY";
    case DataType::Struct: return "STRUCT";
    }
    return "unknown";
}

std::optional<DataType> elementaryTypeFromName(std::string_view name) {
    const auto upper = toUpper(name);
    for (const auto type : kElementaryTypes) {
        if (upper == dataTypeName(type)) {
            return type;
        }
    }
    return std::nullopt;
}

std::span<const DataType> elementaryTypes() { return kElementaryTypes; }

std::string partText(const PartSelector& part) {
    const char letter = part.size == AccessSize::Bit ? 'X' : part.size == AccessSize::Byte ? 'B' : 'W';
    return fmt::format(".%{}{}", letter, part.index);
}

bool isIntegerType(DataType type) {
    switch (type) {
    case DataType::SInt:
    case DataType::USInt:
    case DataType::Int:
    case DataType::UInt:
    case DataType::DInt:
    case DataType::UDInt:
    case DataType::LInt: return true;
    default: return false;
    }
}

bool isUnsignedType(DataType type) {
    return type == DataType::USInt || type == DataType::UInt || type == DataType::UDInt;
}

bool isBitString(DataType type) {
    return type == DataType::Byte || type == DataType::Word || type == DataType::DWord;
}

unsigned bitWidth(DataType type) {
    switch (type) {
    case DataType::Bool: return 1;
    case DataType::SInt:
    case DataType::USInt:
    case DataType::Byte: return 8;
    case DataType::Int:
    case DataType::UInt:
    case DataType::Word: return 16;
    case DataType::DInt:
    case DataType::UDInt:
    case DataType::DWord:
    case DataType::Real:
    case DataType::Time: return 32;
    case DataType::LInt: return 64;
    default: return 0;
    }
}

std::pair<std::int64_t, std::int64_t> integerRange(DataType type) {
    if (type == DataType::LInt || type == DataType::AnyInt) {
        return {std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::int64_t>::max()};
    }
    const unsigned bits = bitWidth(type);
    if (isUnsignedType(type) || isBitString(type)) {
        return {0, (std::int64_t{1} << bits) - 1};
    }
    return {-(std::int64_t{1} << (bits - 1)), (std::int64_t{1} << (bits - 1)) - 1};
}

std::string formatTypeSpec(const TypeSpec& spec) {
    // Elementary types in their usual spelling; other names as written.
    const auto elementary = elementaryTypeFromName(spec.name);
    const std::string name = elementary ? dataTypeName(*elementary) : spec.name;
    if (spec.dimensions.empty()) {
        return name;
    }
    std::string text = "ARRAY[";
    for (std::size_t i = 0; i < spec.dimensions.size(); ++i) {
        text += fmt::format("{}{}..{}", i == 0 ? "" : ", ", spec.dimensions[i].low, spec.dimensions[i].high);
    }
    return text + "] OF " + name;
}

const Type::Field* Type::field(std::string_view fieldName) const {
    const auto upper = toUpper(fieldName);
    for (const auto& f : fields) {
        if (toUpper(f.name) == upper) {
            return &f;
        }
    }
    return nullptr;
}

const Type* elementaryType(DataType type) {
    // One shared Type per elementary type, built on first use.
    static const auto types = [] {
        std::vector<Type> list;
        for (const auto t : kElementaryTypes) {
            Type entry;
            entry.kind = Type::Kind::Elementary;
            entry.name = dataTypeName(t);
            entry.elementary = t;
            entry.bitSize = bitWidth(t);
            entry.leaves.push_back(Type::Leaf{t, 0, ""});
            list.push_back(std::move(entry));
        }
        return list;
    }();
    for (const auto& t : types) {
        if (t.elementary == type) {
            return &t;
        }
    }
    return nullptr;
}

bool sameType(const Type& a, const Type& b) {
    if (&a == &b) {
        return true;
    }
    // Two separately written ARRAY[1..3] OF INT are the same type; two
    // different STRUCT declarations never are, even with the same fields.
    return a.kind == Type::Kind::Array && b.kind == Type::Kind::Array && a.dimensions == b.dimensions &&
           sameType(*a.element, *b.element);
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

// Copies what the parser wrote; the analyzer's results aren't copied (cloning
// is only done before analysis).
Access cloneAccess(const Access& access) {
    Access copy;
    copy.name = access.name;
    for (const auto& selector : access.selectors) {
        std::visit(Overloaded{
                       [&](const FieldSelector& field) { copy.selectors.emplace_back(field); },
                       [&](const PartSelector& part) { copy.selectors.emplace_back(part); },
                       [&](const IndexSelector& index) {
                           IndexSelector indexCopy;
                           indexCopy.location = index.location;
                           for (const auto& i : index.indices) {
                               indexCopy.indices.push_back(cloneExpression(*i));
                           }
                           copy.selectors.emplace_back(std::move(indexCopy));
                       },
                   },
                   selector);
    }
    return copy;
}

std::string formatAccess(const Access& access) {
    std::string text = access.name;
    for (const auto& selector : access.selectors) {
        std::visit(Overloaded{
                       [&](const FieldSelector& field) { text += "." + field.name; },
                       [&](const PartSelector& part) { text += partText(part); },
                       [&](const IndexSelector& index) {
                           text += '[';
                           for (std::size_t i = 0; i < index.indices.size(); ++i) {
                               text += (i == 0 ? "" : ", ") + formatExpression(*index.indices[i]);
                           }
                           text += ']';
                       },
                   },
                   selector);
    }
    return text;
}

InitializerPtr cloneInitializer(const Initializer& initializer) {
    using Node = decltype(Initializer::node);
    auto node = std::visit(Overloaded{
                               [](const ExpressionPtr& expression) -> Node { return cloneExpression(*expression); },
                               [](const ArrayInitializer& array) -> Node {
                                   ArrayInitializer copy;
                                   for (const auto& item : array.items) {
                                       copy.items.push_back(
                                           ArrayInitItem{item.repeat, item.value ? cloneInitializer(*item.value) : nullptr});
                                   }
                                   return copy;
                               },
                               [](const StructInitializer& structure) -> Node {
                                   StructInitializer copy;
                                   for (const auto& field : structure.fields) {
                                       copy.fields.push_back(
                                           StructInitField{field.name, cloneInitializer(*field.value), field.location});
                                   }
                                   return copy;
                               },
                           },
                           initializer.node);
    return std::make_unique<Initializer>(Initializer{std::move(node), initializer.location});
}

std::string formatInitializer(const Initializer& initializer) {
    return std::visit(Overloaded{
                          [](const ExpressionPtr& expression) { return formatExpression(*expression); },
                          [](const ArrayInitializer& array) {
                              std::string text = "[";
                              for (std::size_t i = 0; i < array.items.size(); ++i) {
                                  const auto& item = array.items[i];
                                  text += i == 0 ? "" : ", ";
                                  const auto value = item.value ? formatInitializer(*item.value) : std::string();
                                  text += item.repeat == 1 && item.value ? value : fmt::format("{}({})", item.repeat, value);
                              }
                              return text + "]";
                          },
                          [](const StructInitializer& structure) {
                              std::string text = "(";
                              for (std::size_t i = 0; i < structure.fields.size(); ++i) {
                                  text += fmt::format("{}{} := {}", i == 0 ? "" : ", ", structure.fields[i].name,
                                                      formatInitializer(*structure.fields[i].value));
                              }
                              return text + ")";
                          },
                      },
                      initializer.node);
}

ExpressionPtr cloneExpression(const Expression& expression) {
    using Node = decltype(Expression::node);
    auto node = std::visit(Overloaded{
                               [](const Literal& literal) -> Node { return literal; },
                               [](const Access& access) -> Node { return cloneAccess(access); },
                               [](const CallExpression& call) -> Node {
                                   CallExpression copy;
                                   copy.name = call.name;
                                   for (const auto& argument : call.arguments) {
                                       copy.arguments.push_back(CallArgument{argument.name, cloneExpression(*argument.value),
                                                                             argument.location});
                                   }
                                   return copy;
                               },
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
                          [](const Access& access) { return formatAccess(access); },
                          [](const CallExpression& call) {
                              std::string text = call.name + "(";
                              for (std::size_t i = 0; i < call.arguments.size(); ++i) {
                                  const auto& argument = call.arguments[i];
                                  text += i == 0 ? "" : ", ";
                                  if (!argument.name.empty()) {
                                      text += argument.name + " := ";
                                  }
                                  text += formatExpression(*argument.value);
                              }
                              return text + ")";
                          },
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
    for (const auto& type : unit.types) {
        TreeWriter(out).typeDeclaration(type);
        out += '\n';
    }
    for (const auto& function : unit.functions) {
        out += dumpTree(function);
        out += '\n';
    }
    for (const auto& functionBlock : unit.functionBlocks) {
        out += dumpTree(functionBlock);
        out += '\n';
    }
    out += dumpTree(unit.program);
    return out;
}

} // namespace softplc::st
