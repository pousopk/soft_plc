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

// The most leaves one variable may have: a guard against typing
// ARRAY[1..100000000] by accident.
constexpr std::size_t kMaxLeaves = 1'000'000;

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

// Rounds a bit count up to whole words (16 bits): in the S7 layout, words,
// structs and arrays start at even bytes.
std::size_t roundUpToWord(std::size_t bits) { return (bits + 15) / 16 * 16; }

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

// The aggregate type of an expression, if it is a whole array or struct.
const Type* aggregateOf(const Expression& expression) {
    if (const auto* access = std::get_if<Access>(&expression.node); access && access->type &&
                                                                    access->type->isAggregate()) {
        return access->type;
    }
    return nullptr;
}

// How a type is named in error messages.
std::string typeText(const Expression& expression) {
    if (const auto* aggregate = aggregateOf(expression)) {
        return aggregate->name;
    }
    return dataTypeName(expression.type);
}

// "[1,2]" for the element at `index` (counted from 0) of an array.
std::string indexSuffix(const std::vector<ArrayDimension>& dimensions, std::size_t index) {
    std::vector<std::int64_t> parts(dimensions.size());
    for (std::size_t k = dimensions.size(); k-- > 0;) {
        const auto length = static_cast<std::size_t>(dimensions[k].length());
        parts[k] = dimensions[k].low + static_cast<std::int64_t>(index % length);
        index /= length;
    }
    std::string text = "[";
    for (std::size_t k = 0; k < parts.size(); ++k) {
        text += fmt::format("{}{}", k == 0 ? "" : ",", parts[k]);
    }
    return text + "]";
}

class Analyzer {
public:
    explicit Analyzer(CompilationUnit& unit) : unit_(unit) {}

    void run() {
        registerFunctionBlockTypes();
        registerTypeDeclarations();
        for (auto& type : unit_.types) {
            resolveDeclaredType(type);
        }
        // First every POU's variables, so a body can use any function
        // block's inputs and outputs, wherever that block is declared.
        for (auto& functionBlock : unit_.functionBlocks) {
            declarePou(functionBlock);
        }
        declarePou(unit_.program);
        for (auto& functionBlock : unit_.functionBlocks) {
            checkBody(functionBlock);
        }
        checkBody(unit_.program);
        checkNoFunctionBlockContainsItself();
    }

private:
    [[noreturn]] static void fail(SourceLocation where, std::string message) {
        throw AnalysisError{Diagnostic{std::move(message), where}};
    }

    // ------------------------------------------------------------ Function block types

    void registerFunctionBlockTypes() {
        for (const auto& builtin : builtinFunctionBlocks()) {
            functionBlocks_.emplace(toUpper(builtin.name), &builtin);
        }
        for (const auto& functionBlock : unit_.functionBlocks) {
            const auto [it, inserted] = functionBlocks_.emplace(toUpper(functionBlock.name), &functionBlock);
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

    // ------------------------------------------------------------ Declared types (TYPE ... END_TYPE)

    void registerTypeDeclarations() {
        for (auto& type : unit_.types) {
            const auto upper = toUpper(type.name);
            if (elementaryTypeFromName(type.name)) {
                fail(type.location, fmt::format("'{}' is an elementary type; choose another name", type.name));
            }
            if (const auto fb = functionBlocks_.find(upper); fb != functionBlocks_.end()) {
                fail(type.location, fb->second->builtin
                                        ? fmt::format("'{}' is a standard function block; choose another name", type.name)
                                        : fmt::format("'{}' is already the name of a function block", type.name));
            }
            const auto [it, inserted] = declaredTypes_.emplace(upper, &type);
            if (!inserted) {
                fail(type.location,
                     fmt::format("type '{}' is already defined on line {}", type.name, it->second->location.line));
            }
        }
    }

    // Builds the Type of a TYPE declaration, once. Types may use each other
    // in any order, but never themselves (a Point containing a Point).
    const Type* resolveDeclaredType(TypeDeclaration& declaration) {
        if (const auto done = resolvedTypes_.find(&declaration); done != resolvedTypes_.end()) {
            return done->second;
        }
        if (std::find(resolving_.begin(), resolving_.end(), &declaration) != resolving_.end()) {
            std::string chain;
            for (const auto* step : resolving_) {
                chain += step->name + " -> ";
            }
            fail(declaration.location,
                 fmt::format("type '{}' contains itself: {}{}", declaration.name, chain, declaration.name));
        }
        resolving_.push_back(&declaration);
        const Type* type = declaration.isStruct ? makeStruct(declaration)
                                                : resolveTypeSpec(declaration.alias, declaration.name);
        resolving_.pop_back();
        resolvedTypes_.emplace(&declaration, type);
        return type;
    }

    // A type name used inside a STRUCT, an ARRAY or a TYPE: elementary or declared.
    const Type* resolveNamedType(const std::string& name, SourceLocation where) {
        if (const auto elementary = elementaryTypeFromName(name)) {
            return elementaryType(*elementary);
        }
        if (const auto it = declaredTypes_.find(toUpper(name)); it != declaredTypes_.end()) {
            return resolveDeclaredType(*it->second);
        }
        if (functionBlocks_.count(toUpper(name)) > 0) {
            fail(where, fmt::format("'{}' is a function block: arrays and structs of function block instances "
                                    "aren't supported",
                                    name));
        }
        fail(where, fmt::format("unknown type '{}' (not BOOL, INT, DINT, REAL, TIME, a TYPE or a function block)", name));
    }

    // A type as written: a name, or ARRAY[...] OF a name. `aliasName` names
    // the result when it's the whole of a TYPE declaration (TYPE Row : ARRAY...).
    const Type* resolveTypeSpec(const TypeSpec& spec, const std::string& aliasName = {}) {
        const Type* named = resolveNamedType(spec.name, spec.location);
        if (spec.dimensions.empty()) {
            return named;
        }
        return makeArray(spec, *named, aliasName);
    }

    Type* newType() {
        unit_.analyzedTypes.push_back(std::make_unique<Type>());
        return unit_.analyzedTypes.back().get();
    }

    const Type* makeArray(const TypeSpec& spec, const Type& element, const std::string& aliasName) {
        std::size_t count = 1;
        for (const auto& dimension : spec.dimensions) {
            count *= static_cast<std::size_t>(dimension.length());
            if (count * element.leafCount() > kMaxLeaves) {
                fail(spec.location, fmt::format("{} is too big (more than {} values)", formatTypeSpec(spec), kMaxLeaves));
            }
        }
        Type* type = newType();
        type->kind = Type::Kind::Array;
        type->elementary = DataType::Array;
        type->name = aliasName.empty() ? formatTypeSpec(spec) : aliasName;
        type->dimensions = spec.dimensions;
        type->element = &element;
        type->elementLeaves = element.leafCount();
        // S7 layout: BOOL elements are packed bit after bit; everything else
        // keeps its own size (structs and arrays are already whole words).
        type->elementBits = element.kind == Type::Kind::Elementary && element.elementary == DataType::Bool
                                ? 1
                                : element.bitSize;
        type->bitSize = roundUpToWord(count * type->elementBits);
        type->leaves.reserve(count * element.leafCount());
        for (std::size_t index = 0; index < count; ++index) {
            const auto suffix = indexSuffix(spec.dimensions, index);
            for (const auto& leaf : element.leaves) {
                type->leaves.push_back(
                    Type::Leaf{leaf.type, index * type->elementBits + leaf.bitOffset, suffix + leaf.suffix});
            }
        }
        return type;
    }

    const Type* makeStruct(TypeDeclaration& declaration) {
        Type* type = newType();
        type->kind = Type::Kind::Struct;
        type->elementary = DataType::Struct;
        type->name = declaration.name;

        std::set<std::string> names;
        std::size_t bit = 0;
        for (auto& field : declaration.fields) {
            if (!names.insert(toUpper(field.name)).second) {
                fail(field.location, fmt::format("STRUCT '{}' already has a field '{}'", declaration.name, field.name));
            }
            if (field.address) {
                fail(field.location, "a STRUCT field can't have an AT address; place the whole variable with AT");
            }
            const Type* fieldType = resolveTypeSpec(field.typeSpec);
            field.typeInfo = fieldType;
            field.type = fieldType->elementary;

            // S7 layout: BOOLs take the next free bit; everything else starts at an even byte.
            const bool isBool = fieldType->kind == Type::Kind::Elementary && fieldType->elementary == DataType::Bool;
            if (!isBool) {
                bit = roundUpToWord(bit);
            }
            type->fields.push_back(Type::Field{field.name, fieldType, type->leaves.size(), bit, nullptr});
            for (const auto& leaf : fieldType->leaves) {
                type->leaves.push_back(Type::Leaf{leaf.type, bit + leaf.bitOffset, "." + field.name + leaf.suffix});
            }
            bit += fieldType->bitSize;
            if (type->leaves.size() > kMaxLeaves) {
                fail(field.location, fmt::format("STRUCT '{}' is too big (more than {} values)", declaration.name, kMaxLeaves));
            }
        }
        type->bitSize = roundUpToWord(bit);

        // Default values, checked once the struct itself is complete.
        for (std::size_t i = 0; i < declaration.fields.size(); ++i) {
            auto& field = declaration.fields[i];
            if (field.initialValue) {
                checkInitializer(*field.typeInfo, *field.initialValue, field.name);
                type->fields[i].initialValue = field.initialValue.get();
            }
        }
        return type;
    }

    // ------------------------------------------------------------ Declaring one POU's variables

    // Sets pou_ and the name -> slot table for one POU.
    void enterPou(Pou& pou) {
        pou_ = &pou;
        slots_.clear();
        for (std::size_t slot = 0; slot < pou.variables.size(); ++slot) {
            slots_.emplace(toUpper(pou.variables[slot].name), slot);
        }
    }

    bool inProgram() const { return pou_->kind == PouKind::Program; }

    void declarePou(Pou& pou) {
        pou_ = &pou;
        slots_.clear();
        std::size_t leaf = 0;
        for (std::size_t slot = 0; slot < pou.variables.size(); ++slot) {
            auto& variable = pou.variables[slot];
            const auto [it, inserted] = slots_.emplace(toUpper(variable.name), slot);
            if (!inserted) {
                const auto& first = pou.variables[it->second];
                fail(variable.location,
                     fmt::format("'{}' is already declared on line {}", variable.name, first.location.line));
            }
            if (inProgram() && variable.section != VarSection::Local) {
                fail(variable.location, "a PROGRAM can only have VAR blocks; VAR_INPUT and VAR_OUTPUT are for "
                                        "function blocks");
            }
            resolveVariableType(variable);
            if (variable.address) {
                checkAddress(variable);
            }
            if (variable.initialValue) {
                checkInitializer(*variable.typeInfo, *variable.initialValue, variable.name);
            }
            // Leaves: the variable's values inside the instance. Function
            // block instances have their own memory, and AT variables live
            // in the process image.
            if (variable.type != DataType::FunctionBlock && !variable.address) {
                variable.leaf = leaf;
                leaf += variable.typeInfo->leafCount();
            }
        }
        pou.leafCount = leaf;
    }

    void resolveVariableType(VariableDeclaration& variable) {
        const auto& spec = variable.typeSpec;
        if (spec.dimensions.empty()) {
            if (const auto fb = functionBlocks_.find(toUpper(spec.name)); fb != functionBlocks_.end()) {
                variable.type = DataType::FunctionBlock;
                variable.functionBlock = fb->second;
                if (variable.section != VarSection::Local) {
                    fail(variable.location, "inputs and outputs can't be function block instances");
                }
                if (variable.initialValue) {
                    fail(variable.location, fmt::format("'{}' is a {} instance and can't have an initial value",
                                                        variable.name, fb->second->name));
                }
                return;
            }
        }
        variable.typeInfo = resolveTypeSpec(spec);
        variable.type = variable.typeInfo->elementary;
    }

    void checkAddress(const VariableDeclaration& variable) const {
        if (!inProgram()) {
            fail(variable.location, fmt::format("AT can only be used in the PROGRAM, not inside function block '{}'",
                                                pou_->name));
        }
        const auto& address = *variable.address;
        if (variable.type == DataType::FunctionBlock) {
            fail(variable.location, fmt::format("{} '{}' needs no address at all, not {}", variable.typeSpec.name,
                                                variable.name, address.toString()));
        }
        if (variable.initialValue) {
            fail(variable.location, fmt::format("'{}' can't have an initial value: its value comes from {}",
                                                variable.name, address.toString()));
        }
        if (variable.typeInfo->isAggregate()) {
            // Like S7: arrays and structs start at an even byte.
            const bool startsAtByte = address.size == AccessSize::Byte ||
                                      (address.size == AccessSize::Bit && address.bit == 0);
            if (!startsAtByte || address.byteOffset % 2 != 0) {
                fail(variable.location, fmt::format("{} '{}' needs an even byte address like %MB10 or %DB1.DBB0, not {}",
                                                    variable.typeInfo->name, variable.name, address.toString()));
            }
            return;
        }
        const auto size = address.size;
        const bool fits = (variable.type == DataType::Bool && size == AccessSize::Bit) ||
                          (variable.type == DataType::Int && size == AccessSize::Word) ||
                          ((variable.type == DataType::DInt || variable.type == DataType::Real ||
                            variable.type == DataType::Time) &&
                           size == AccessSize::DWord);
        if (!fits) {
            const char* needed = variable.type == DataType::Bool  ? "a bit address like %IX0.0"
                                 : variable.type == DataType::Int ? "a word address like %MW0"
                                                                  : "a double word address like %MD0";
            fail(variable.location, fmt::format("{} '{}' needs {}, not {}", dataTypeName(variable.type), variable.name,
                                                needed, address.toString()));
        }
    }

    // Checks an initial value against the type it initializes: constants
    // only, the right shape ([...] for arrays, (...) for structs), and values
    // that fit.
    void checkInitializer(const Type& type, Initializer& initializer, const std::string& name) {
        std::visit(Overloaded{
                       [&](ExpressionPtr& expression) {
                           if (type.isAggregate()) {
                               fail(initializer.location,
                                    fmt::format("'{}' is {}: write its initial values as {}", name, type.name,
                                                type.kind == Type::Kind::Array ? "[1, 2, 3]" : "(field := value, ...)"));
                           }
                           constantsOnly_ = true;
                           checkExpression(*expression);
                           constantsOnly_ = false;
                           checkAssignable(type, *expression, name);
                       },
                       [&](ArrayInitializer& array) {
                           if (type.kind != Type::Kind::Array) {
                               fail(initializer.location, fmt::format("'{}' is {}, not an array, so it can't have [...] "
                                                                      "initial values",
                                                                      name, type.name));
                           }
                           std::size_t count = 0;
                           for (auto& item : array.items) {
                               count += static_cast<std::size_t>(item.repeat);
                               if (item.value) {
                                   checkInitializer(*type.element, *item.value, name + "[...]");
                               }
                           }
                           const auto elements = type.leafCount() / std::max<std::size_t>(type.elementLeaves, 1);
                           if (count > elements) {
                               fail(initializer.location, fmt::format("too many initial values for '{}': {} for {} "
                                                                      "elements",
                                                                      name, count, elements));
                           }
                       },
                       [&](StructInitializer& structure) {
                           if (type.kind != Type::Kind::Struct) {
                               fail(initializer.location, fmt::format("'{}' is {}, not a struct, so it can't have "
                                                                      "(field := ...) initial values",
                                                                      name, type.name));
                           }
                           std::set<std::string> given;
                           for (auto& field : structure.fields) {
                               const auto* f = type.field(field.name);
                               if (f == nullptr) {
                                   fail(field.location, fmt::format("{} has no field '{}'", type.name, field.name));
                               }
                               if (!given.insert(toUpper(field.name)).second) {
                                   fail(field.location, fmt::format("'{}' is given twice", field.name));
                               }
                               checkInitializer(*f->type, *field.value, name + "." + f->name);
                           }
                       },
                   },
                   initializer.node);
    }

    // ------------------------------------------------------------ One POU's body

    void checkBody(Pou& pou) {
        enterPou(pou);
        loopVariables_.clear();
        loopDepth_ = 0;
        checkStatements(pou.body);
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

    // Can a value of `value`'s type be stored in something of type `target`
    // without losing anything?
    void checkAssignable(const Type& target, const Expression& value, const std::string& targetName) const {
        const auto* aggregate = aggregateOf(value);
        if (target.isAggregate() || aggregate != nullptr) {
            if (target.isAggregate() && aggregate != nullptr && sameType(target, *aggregate)) {
                return;
            }
            fail(value.location, fmt::format("can't assign {} to '{}', which is {}", typeText(value), targetName,
                                             target.name));
        }
        const auto source = value.type;
        const auto targetType = target.elementary;
        if (source == targetType) {
            return;
        }
        if (source == DataType::AnyInt && (targetType == DataType::Int || targetType == DataType::DInt)) {
            checkLiteralFits(targetType, value);
            return;
        }
        const bool widening = (targetType == DataType::DInt && source == DataType::Int) ||
                              (targetType == DataType::Real && isNumeric(source));
        if (!widening) {
            const char* hint = isNumeric(source) && isNumeric(targetType) ? " (values might not fit)"
                               : targetType == DataType::Time            ? " (write times like T#500ms)"
                                                                         : "";
            fail(value.location, fmt::format("can't assign {} to '{}', which is {}{}", dataTypeName(source),
                                             targetName, dataTypeName(targetType), hint));
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

    // ------------------------------------------------------------ Accesses: a, t1.Q, values[i].x

    // Resolves a variable or a part of one, and returns the type of the part.
    const Type& resolveAccess(Access& access, SourceLocation where) {
        if (constantsOnly_) {
            fail(where, fmt::format("initial values must be constants, not variables like '{}'", access.name));
        }
        const auto slot = lookup(access.name, where);
        const VariableDeclaration* variable = &variableAt(slot);
        std::size_t next = 0; // the first selector still to apply

        if (variable->type == DataType::FunctionBlock) {
            const Pou* block = variable->functionBlock;
            const auto* field = access.selectors.empty() ? nullptr : std::get_if<FieldSelector>(&access.selectors[0]);
            if (field == nullptr) {
                fail(where, fmt::format("'{}' is a {} instance, not a value; use one of its outputs, like {}.Q",
                                        access.name, block->name, access.name));
            }
            const auto member = findVariable(*block, field->name);
            if (!member) {
                fail(where, fmt::format("{} has no input or output called '{}'", block->name, field->name));
            }
            const auto& declaration = block->variables[*member];
            if (declaration.section == VarSection::Local) {
                fail(where, fmt::format("'{}' is internal to {}; only its inputs and outputs can be read",
                                        declaration.name, block->name));
            }
            access.instanceSlot = slot;
            access.slot = *member;
            variable = &declaration;
            next = 1;
        } else {
            access.slot = slot;
        }

        const Type* type = variable->typeInfo;
        std::string path = next == 0 ? variable->name : access.name + "." + variable->name;
        access.leafOffset = 0;
        access.bitOffset = 0;
        access.dynamic.clear();
        for (; next < access.selectors.size(); ++next) {
            std::visit(Overloaded{
                           [&](const FieldSelector& field) {
                               if (type->kind != Type::Kind::Struct) {
                                   fail(field.location,
                                        next == 0 && type->kind == Type::Kind::Elementary
                                            ? fmt::format("'{}' is not a function block instance or a struct, so it "
                                                          "has no members",
                                                          path)
                                            : fmt::format("'{}' is {}, not a struct, so it has no field '{}'", path,
                                                          type->name, field.name));
                               }
                               const auto* f = type->field(field.name);
                               if (f == nullptr) {
                                   fail(field.location, fmt::format("{} has no field '{}'", type->name, field.name));
                               }
                               access.leafOffset += f->leafOffset;
                               access.bitOffset += f->bitOffset;
                               path += "." + f->name;
                               type = f->type;
                           },
                           [&](IndexSelector& index) {
                               if (type->kind != Type::Kind::Array) {
                                   fail(index.location, fmt::format("'{}' is {}, not an array", path, type->name));
                               }
                               if (index.indices.size() != type->dimensions.size()) {
                                   fail(index.location, fmt::format("'{}' is {}: it needs {} index{}, not {}", path,
                                                                    type->name, type->dimensions.size(),
                                                                    type->dimensions.size() == 1 ? "" : "es",
                                                                    index.indices.size()));
                               }
                               // Row-major: the last index moves fastest.
                               std::size_t elementsPerStep = 1;
                               for (std::size_t k = type->dimensions.size(); k-- > 0;) {
                                   const auto& bounds = type->dimensions[k];
                                   auto& expression = *index.indices[k];
                                   requireInteger(expression, "an array index");
                                   const auto leafStride = elementsPerStep * type->elementLeaves;
                                   const auto bitStride = elementsPerStep * type->elementBits;
                                   if (const auto constant = integerConstant(expression)) {
                                       if (*constant < bounds.low || *constant > bounds.high) {
                                           fail(expression.location,
                                                fmt::format("index {} is outside the bounds of '{}' ({}..{})", *constant,
                                                            path, bounds.low, bounds.high));
                                       }
                                       const auto steps = static_cast<std::size_t>(*constant - bounds.low);
                                       access.leafOffset += steps * leafStride;
                                       access.bitOffset += steps * bitStride;
                                   } else {
                                       access.dynamic.push_back(IndexTerm{&expression, bounds, leafStride, bitStride});
                                   }
                                   elementsPerStep *= static_cast<std::size_t>(bounds.length());
                               }
                               path += "[...]";
                               type = type->element;
                           },
                       },
                       access.selectors[next]);
        }
        access.type = type;
        access.directLeaf = access.dynamic.empty() && !variable->address ? variable->leaf + access.leafOffset : kNoSlot;
        return *type;
    }

    // ------------------------------------------------------------ Expressions

    // Works out and records the type of an expression and all its children.
    DataType checkExpression(Expression& expression) {
        expression.type = std::visit(Overloaded{
                                         [&](Literal& literal) { return literalType(literal); },
                                         [&](Access& access) { return resolveAccess(access, expression.location).elementary; },
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

    DataType unaryType(UnaryExpression& unary, const Expression& expression) {
        const auto operand = checkExpression(*unary.operand);
        if (unary.op == UnaryOperator::Negate) {
            if (!isNumeric(operand) && operand != DataType::Time) {
                fail(expression.location, fmt::format("'-' needs a number, not {}", typeText(*unary.operand)));
            }
            return operand;
        }
        if (operand != DataType::Bool) {
            fail(expression.location, fmt::format("NOT needs a BOOL, not {}", typeText(*unary.operand)));
        }
        return DataType::Bool;
    }

    DataType binaryType(BinaryExpression& binary, const Expression& expression) {
        const auto left = checkExpression(*binary.left);
        const auto right = checkExpression(*binary.right);
        const auto symbol = operatorSymbol(binary.op);
        const auto mismatch = [&](const char* needs) {
            fail(expression.location, fmt::format("'{}' needs {}, but got {} and {}", symbol, needs,
                                                  typeText(*binary.left), typeText(*binary.right)));
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
            fail(condition.location, fmt::format("{} must be BOOL, not {}", what, typeText(condition)));
        }
    }

    void requireInteger(Expression& expression, const char* what) {
        const auto type = checkExpression(expression);
        if (!isInteger(type)) {
            fail(expression.location, fmt::format("{} must be an integer, not {}", what, typeText(expression)));
        }
    }

    // ------------------------------------------------------------ Statements

    void checkStatements(StatementList& list) {
        for (auto& statement : list) {
            std::visit([&](auto& node) { check(node, statement.location); }, statement.node);
        }
    }

    void check(Assignment& s, SourceLocation where) {
        auto& target = s.target;
        const auto slot = lookup(target.name, where);
        const auto& variable = variableAt(slot);
        if (variable.type == DataType::FunctionBlock) {
            if (target.selectors.empty()) {
                fail(where, fmt::format("can't assign to '{}': it's a {} instance", target.name,
                                        variable.functionBlock->name));
            }
            fail(where, fmt::format("can't assign to '{}': set inputs in the call, like {}(IN := ...); outputs are "
                                    "read-only",
                                    formatAccess(target), target.name));
        }
        const auto& type = resolveAccess(target, where);
        if (target.isPlainVariable() &&
            std::find(loopVariables_.begin(), loopVariables_.end(), target.slot) != loopVariables_.end()) {
            fail(where, fmt::format("'{}' is the variable of a FOR loop and can't be changed inside it", target.name));
        }
        checkExpression(*s.value);
        checkAssignable(type, *s.value, formatAccess(target));
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
            checkAssignable(*input.typeInfo, *argument.value, s.instance + "." + input.name);
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
            const auto text = variable.typeInfo ? variable.typeInfo->name : variable.typeSpec.name;
            fail(where, fmt::format("the FOR variable '{}' must be INT or DINT, not {}", s.variable, text));
        }
        for (Expression* bound : {s.from.get(), s.to.get(), s.step.get()}) {
            if (bound != nullptr) {
                requireInteger(*bound, "a FOR loop bound");
                checkAssignable(*variable.typeInfo, *bound, s.variable);
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
    std::unordered_map<std::string, const Pou*> functionBlocks_;          // upper-case name -> user or standard block
    std::unordered_map<std::string, TypeDeclaration*> declaredTypes_;     // upper-case name -> TYPE declaration
    std::unordered_map<const TypeDeclaration*, const Type*> resolvedTypes_;
    std::vector<const TypeDeclaration*> resolving_; // the TYPE declarations being built, to catch cycles

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

std::map<std::uint16_t, std::size_t> dataBlocksUsedBy(const CompilationUnit& unit) {
    std::map<std::uint16_t, std::size_t> sizes;
    for (const auto& variable : unit.program.variables) { // AT is only allowed in the PROGRAM
        if (!variable.address || variable.address->area != Area::DataBlock) {
            continue;
        }
        const auto& a = *variable.address;
        std::size_t width = a.size == AccessSize::Word ? 2 : a.size == AccessSize::DWord ? 4 : 1;
        if (variable.typeInfo != nullptr && variable.typeInfo->isAggregate()) {
            width = variable.typeInfo->bitSize / 8;
        }
        auto& size = sizes[a.dbNumber];
        size = std::max(size, a.byteOffset + width);
    }
    return sizes;
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
