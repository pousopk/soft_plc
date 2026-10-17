#include "st/interpreter.hpp"
#include "st/builtins.hpp"
#include "st/compiler.hpp"
#include "st/runtime_values.hpp"
#include "util/overloaded.hpp"
#include "util/text.hpp"
#include <bit>
#include <charconv>
#include <cmath>
#include <utility>
#include <fmt/format.h>
#include <vector>

namespace softplc::st {

// The memory of one POU instance.
struct Instance {
    const Pou* type = nullptr;
    // The leaves of every variable of `type`, in declaration order (see
    // VariableDeclaration::leaf). Function block variables have none (see
    // `children`), and neither do AT variables: they live in the process image.
    std::vector<Value> values;
    // One entry per variable; set only for function block variables.
    std::vector<std::unique_ptr<Instance>> children;
    // Set only for standard blocks like TON: their behaviour in C++.
    std::unique_ptr<NativeBlock> native;
};

namespace {

using namespace runtime;

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

// The message for an array index out of bounds, the same for both engines.
std::string indexError(std::int64_t index, const ArrayDimension& bounds) {
    return fmt::format("array index {} is outside the bounds {}..{}", index, bounds.low, bounds.high);
}

// The process image address of one leaf of an AT variable, `bit` bits from its start.
Address leafAddress(const VariableDeclaration& variable, std::size_t bit, DataType type) {
    const auto& start = *variable.address;
    const auto first = start.byteOffset * 8 + (start.size == AccessSize::Bit ? start.bit : 0);
    Address address = start;
    address.byteOffset = (first + bit) / 8;
    address.bit = static_cast<unsigned>((first + bit) % 8);
    address.size = type == DataType::Bool ? AccessSize::Bit : type == DataType::Int ? AccessSize::Word : AccessSize::DWord;
    return address;
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

const char* engineName(Engine engine) { return engine == Engine::TreeWalker ? "tree" : "bytecode"; }

Interpreter::Interpreter(CompilationUnit unit, std::uint64_t loopLimit, Engine engine)
    : unit_(std::make_unique<CompilationUnit>(std::move(unit))), loopLimit_(loopLimit), engine_(engine) {
    if (engine_ == Engine::Bytecode) {
        // Compile every POU once, up front: cycles then only run the bytecode.
        chunks_.emplace(&unit_->program, compile(unit_->program));
        for (const auto& functionBlock : unit_->functionBlocks) {
            chunks_.emplace(&functionBlock, compile(functionBlock));
        }
        vmStack_.resize(16 * 1024);
    }
    root_ = createInstance(unit_->program);
}

const Chunk* Interpreter::bytecode(const Pou& pou) const {
    const auto it = chunks_.find(&pou);
    return it == chunks_.end() ? nullptr : &it->second;
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
    instance->values.resize(pou.leafCount);
    for (std::size_t slot = 0; slot < pou.variables.size(); ++slot) {
        const auto& variable = pou.variables[slot];
        if (variable.type == DataType::FunctionBlock) {
            instance->children[slot] = createInstance(*variable.functionBlock);
        } else if (!variable.address) {
            initialize(*instance, *variable.typeInfo, variable.leaf, variable.initialValue.get());
        }
    }
    if (pou.builtin) {
        instance->native = createBuiltin(pou.name);
    }
    return instance;
}

// Sets the leaves of one value of `type`, starting at `leaf`: from the
// initializer where it gives a value, from the STRUCT's field defaults
// elsewhere, and 0 / FALSE for everything else. Initial values are constants
// (the analyzer checked), so evaluating them needs no process image.
void Interpreter::initialize(Instance& instance, const Type& type, std::size_t leaf, const Initializer* initializer) {
    switch (type.kind) {
    case Type::Kind::Elementary: {
        const auto* expression = initializer != nullptr ? std::get_if<ExpressionPtr>(&initializer->node) : nullptr;
        instance.values[leaf] = expression != nullptr ? convert(evaluate(**expression, instance), type.elementary)
                                                      : defaultValue(type.elementary);
        return;
    }
    case Type::Kind::Struct: {
        const auto* given = initializer != nullptr ? std::get_if<StructInitializer>(&initializer->node) : nullptr;
        for (const auto& field : type.fields) {
            const Initializer* value = field.initialValue;
            if (given != nullptr) {
                for (const auto& f : given->fields) {
                    if (toUpper(f.name) == toUpper(field.name)) {
                        value = f.value.get();
                    }
                }
            }
            initialize(instance, *field.type, leaf + field.leafOffset, value);
        }
        return;
    }
    case Type::Kind::Array: {
        std::vector<const Initializer*> values; // one per element, in order; null: the default
        if (const auto* given = initializer != nullptr ? std::get_if<ArrayInitializer>(&initializer->node) : nullptr) {
            for (const auto& item : given->items) {
                values.insert(values.end(), static_cast<std::size_t>(item.repeat), item.value.get());
            }
        }
        const auto count = type.leafCount() / type.elementLeaves;
        for (std::size_t e = 0; e < count; ++e) {
            initialize(instance, *type.element, leaf + e * type.elementLeaves, e < values.size() ? values[e] : nullptr);
        }
        return;
    }
    }
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
    } else if (engine_ == Engine::Bytecode) {
        runBytecode(chunks_.at(instance.type), instance);
    } else {
        execute(instance.type->body, instance); // a RETURN just ends this run early
    }
}

namespace {

// A variable found by path: where it is, its type and its spelling as declared.
struct Resolved {
    Instance* instance;
    const VariableDeclaration* variable;
    std::size_t leaf;
    std::size_t bit;
    DataType type;
    std::string path;
};

// Reads an optionally signed decimal number from the front of `text`.
std::optional<std::int64_t> takeNumber(std::string_view& text) {
    std::int64_t value = 0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || ptr == text.data()) {
        return std::nullopt;
    }
    text.remove_prefix(static_cast<std::size_t>(ptr - text.data()));
    return value;
}

// Walks a path like "fast.offTimer.ET" or "line[2].start.x" down the
// instance tree and into the variable: every name before the variable must
// be a function block instance, and the path must end at one BOOL, INT,
// DINT, REAL or TIME (not a whole array or struct).
std::optional<Resolved> resolvePath(Instance& root, std::string_view path) {
    Instance* instance = &root;
    std::string spelled;
    const VariableDeclaration* variable = nullptr;
    for (;;) {
        const auto end = path.find_first_of(".[");
        const auto slot = findVariable(*instance->type, path.substr(0, end));
        if (!slot) {
            return std::nullopt;
        }
        variable = &instance->type->variables[*slot];
        spelled += variable->name;
        path.remove_prefix(end == std::string_view::npos ? path.size() : end);
        if (variable->type != DataType::FunctionBlock) {
            break;
        }
        if (path.empty() || path.front() != '.') {
            return std::nullopt; // an instance has no single value
        }
        path.remove_prefix(1);
        spelled += '.';
        instance = instance->children[*slot].get();
    }

    const Type* type = variable->typeInfo;
    std::size_t leaf = 0;
    std::size_t bit = 0;
    while (!path.empty()) {
        if (path.front() == '.') {
            path.remove_prefix(1);
            const auto end = path.find_first_of(".[");
            const auto* field = type->kind == Type::Kind::Struct ? type->field(path.substr(0, end)) : nullptr;
            if (field == nullptr) {
                return std::nullopt;
            }
            spelled += "." + field->name;
            leaf += field->leafOffset;
            bit += field->bitOffset;
            type = field->type;
            path.remove_prefix(end == std::string_view::npos ? path.size() : end);
        } else if (path.front() == '[' && type->kind == Type::Kind::Array) {
            path.remove_prefix(1);
            std::vector<std::int64_t> indices;
            for (;;) {
                const auto index = takeNumber(path);
                if (!index) {
                    return std::nullopt;
                }
                indices.push_back(*index);
                if (!path.empty() && path.front() == ',') {
                    path.remove_prefix(1);
                    continue;
                }
                break;
            }
            if (path.empty() || path.front() != ']' || indices.size() != type->dimensions.size()) {
                return std::nullopt;
            }
            path.remove_prefix(1);
            std::size_t element = 0;
            std::string text = "[";
            for (std::size_t k = 0; k < indices.size(); ++k) {
                const auto& bounds = type->dimensions[k];
                if (indices[k] < bounds.low || indices[k] > bounds.high) {
                    return std::nullopt;
                }
                element = element * static_cast<std::size_t>(bounds.length()) +
                          static_cast<std::size_t>(indices[k] - bounds.low);
                text += fmt::format("{}{}", k == 0 ? "" : ",", indices[k]);
            }
            spelled += text + "]";
            leaf += element * type->elementLeaves;
            bit += element * type->elementBits;
            type = type->element;
        } else {
            return std::nullopt;
        }
    }
    if (type->isAggregate()) {
        return std::nullopt; // a whole array or struct has no single value
    }
    return Resolved{instance, variable, leaf, bit, type->elementary, spelled};
}

void collectVariables(const Instance& instance, const std::string& prefix, std::vector<VariableInfo>& out) {
    const auto& variables = instance.type->variables;
    for (std::size_t slot = 0; slot < variables.size(); ++slot) {
        const auto& variable = variables[slot];
        if (variable.type == DataType::FunctionBlock) {
            collectVariables(*instance.children[slot], prefix + variable.name + ".", out);
            continue;
        }
        for (const auto& leaf : variable.typeInfo->leaves) {
            out.push_back(VariableInfo{prefix + variable.name + leaf.suffix, leaf.type});
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
    const auto found = resolvePath(*root_, path);
    if (!found) {
        return std::nullopt;
    }
    return VariableInfo{found->path, found->type};
}

std::optional<Value> Interpreter::valueOf(std::string_view path, const ProcessImage& image) const {
    const auto found = resolvePath(*root_, path);
    if (!found) {
        return std::nullopt;
    }
    return loadLeaf(Place{found->instance, found->variable, found->leaf, found->bit}, found->type, &image);
}

bool Interpreter::setValue(std::string_view path, const Value& value, ProcessImage& image) {
    const auto found = resolvePath(*root_, path);
    if (!found) {
        return false;
    }
    ImageBinding binding(image_, image); // AT variables write to the process image
    storeLeaf(Place{found->instance, found->variable, found->leaf, found->bit}, found->type, value);
    return true;
}

// ---------------------------------------------------------------- Variables

Interpreter::Place Interpreter::wholeVariable(Instance& instance, std::size_t slot) {
    return Place{&instance, &instance.type->variables[slot], 0, 0};
}

Interpreter::Place Interpreter::place(const Access& access, Instance& self) {
    Instance* instance = access.instanceSlot == kNoSlot ? &self : self.children[access.instanceSlot].get();
    Place result{instance, &instance->type->variables[access.slot], access.leafOffset, access.bitOffset};
    for (const auto& term : access.dynamic) {
        const auto index = toInteger(evaluate(*term.index, self));
        if (index < term.bounds.low || index > term.bounds.high) {
            throw RuntimeError{indexError(index, term.bounds), term.index->location};
        }
        const auto steps = static_cast<std::size_t>(index - term.bounds.low);
        result.leaf += steps * term.leafStride;
        result.bit += steps * term.bitStride;
    }
    return result;
}

Value Interpreter::loadLeaf(const Place& place, DataType type, const ProcessImage* image) const {
    const auto& variable = *place.variable;
    if (!variable.address) {
        return place.instance->values[variable.leaf + place.leaf];
    }
    // AT variables: the analyzer made sure the address fits the type, and
    // the S7 layout puts every leaf at an address of the right size.
    const auto raw = image->read(leafAddress(variable, place.bit, type));
    switch (type) {
    case DataType::Bool: return raw != 0;
    case DataType::Int: return std::int64_t{static_cast<std::int16_t>(raw)};
    case DataType::DInt:
    case DataType::Time: return std::int64_t{static_cast<std::int32_t>(raw)};
    case DataType::Real:
        // The 32 bits in memory *are* the float: reinterpret them, don't convert the number.
        return static_cast<double>(std::bit_cast<float>(raw));
    default: return defaultValue(type);
    }
}

void Interpreter::storeLeaf(const Place& place, DataType type, const Value& value) {
    const auto& variable = *place.variable;
    const auto converted = convert(value, type);
    if (!variable.address) {
        place.instance->values[variable.leaf + place.leaf] = converted;
        return;
    }
    std::uint32_t raw = 0;
    switch (type) {
    case DataType::Bool: raw = std::get<bool>(converted) ? 1u : 0u; break;
    case DataType::Real: raw = std::bit_cast<std::uint32_t>(static_cast<float>(std::get<double>(converted))); break;
    default: raw = static_cast<std::uint32_t>(std::get<std::int64_t>(converted)); break; // INT, DINT, TIME
    }
    image_->write(leafAddress(variable, place.bit, type), raw);
}

// A whole array or struct, leaf by leaf (the analyzer checked both sides
// have the same type).
void Interpreter::copyAggregate(const Place& from, const Place& to, const Type& type) {
    for (std::size_t k = 0; k < type.leafCount(); ++k) {
        const auto& leaf = type.leaves[k];
        const auto value = loadLeaf(Place{from.instance, from.variable, from.leaf + k, from.bit + leaf.bitOffset},
                                    leaf.type, image_);
        storeLeaf(Place{to.instance, to.variable, to.leaf + k, to.bit + leaf.bitOffset}, leaf.type, value);
    }
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
                // The value first, then where it goes (the VM does the same).
                if (s.target.type->isAggregate()) {
                    const auto from = place(std::get<Access>(s.value->node), self);
                    copyAggregate(from, place(s.target, self), *s.target.type);
                } else if (s.target.directLeaf != kNoSlot) {
                    self.values[s.target.directLeaf] = convert(evaluate(*s.value, self), s.target.type->elementary);
                } else {
                    const auto value = evaluate(*s.value, self);
                    storeLeaf(place(s.target, self), s.target.type->elementary, value);
                }
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
        const auto input = wholeVariable(instance, argument.slot);
        const auto& type = *input.variable->typeInfo;
        if (type.isAggregate()) {
            copyAggregate(place(std::get<Access>(argument.value->node), self), input, type);
        } else {
            storeLeaf(input, type.elementary, evaluate(*argument.value, self));
        }
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
    const auto variable = wholeVariable(self, s.slot);

    storeLeaf(variable, type, evaluate(*s.from, self));
    for (;;) {
        const auto current = toInteger(loadLeaf(variable, type, image_));
        if (step > 0 ? current > to : current < to) {
            return Flow::Normal;
        }
        countLoopIteration(where);
        const auto flow = execute(s.body, self);
        if (flow == Flow::Exit) return Flow::Normal;
        if (flow == Flow::Return) return Flow::Return;
        // Wraps like the PLC would: FOR i := 0 TO 32767 never ends with an INT
        // (i wraps from 32767 to -32768), which the watchdog then catches.
        storeLeaf(variable, type, wrapInteger(add(current, step), type));
    }
}

void Interpreter::countLoopIteration(SourceLocation where) {
    if (++loopIterations_ > loopLimit_) {
        throw RuntimeError{fmt::format("watchdog: more than {} loop iterations in one cycle", loopLimit_), where};
    }
}

// ---------------------------------------------------------------- The bytecode VM

namespace {

VmSlot toSlot(const Value& value) {
    VmSlot slot{};
    if (const auto* b = std::get_if<bool>(&value)) {
        slot.i = *b ? 1 : 0;
    } else if (const auto* i = std::get_if<std::int64_t>(&value)) {
        slot.i = *i;
    } else {
        slot.r = std::get<double>(value);
    }
    return slot;
}

Value fromSlot(VmSlot slot, DataType type) {
    switch (type) {
    case DataType::Bool: return slot.i != 0;
    case DataType::Real: return slot.r;
    default: return slot.i;
    }
}

} // namespace

// The heart of the VM: fetch an instruction, do what it says, repeat. `sp`
// points just past the top of the operand stack; `temps` are the compiler's
// scratch slots. Everything is checked by the analyzer and laid out by the
// compiler beforehand, so this loop does no type checks and no name lookups.
void Interpreter::runBytecode(const Chunk& chunk, Instance& self) {
    const std::size_t frameSize = chunk.temps + chunk.maxStack;
    if (vmTop_ + frameSize > vmStack_.size()) {
        throw RuntimeError{"VM stack overflow: function blocks nested too deeply", {}};
    }
    VmSlot* const temps = vmStack_.data() + vmTop_;
    VmSlot* sp = temps + chunk.temps;

    // Reserve this frame; nested calls build theirs above it. The guard gives
    // the space back on every way out, exceptions included (RAII).
    struct FrameGuard {
        std::size_t& top;
        std::size_t saved;
        ~FrameGuard() { top = saved; }
    } guard{vmTop_, vmTop_};
    vmTop_ += frameSize;

    auto& values = self.values;
    const Instruction* const code = chunk.code.data();
    std::size_t pc = 0;
    const auto where = [&] { return chunk.locations[pc - 1]; }; // location of the current instruction

    for (;;) {
        const Instruction& in = code[pc++];
        switch (in.op) {
        case Op::PushInt: (sp++)->i = in.imm; break;
        case Op::PushReal: (sp++)->r = std::bit_cast<double>(in.imm); break;

        case Op::LoadBool: (sp++)->i = std::get<bool>(values[in.a]) ? 1 : 0; break;
        case Op::LoadInt: (sp++)->i = std::get<std::int64_t>(values[in.a]); break;
        case Op::LoadReal: (sp++)->r = std::get<double>(values[in.a]); break;
        case Op::StoreBool: values[in.a] = (--sp)->i != 0; break;
        case Op::StoreInt16: values[in.a] = std::int64_t{static_cast<std::int16_t>((--sp)->i)}; break;
        case Op::StoreInt32: values[in.a] = std::int64_t{static_cast<std::int32_t>((--sp)->i)}; break;
        case Op::StoreReal: values[in.a] = roundToReal((--sp)->r); break;

        case Op::LoadMemberBool: (sp++)->i = std::get<bool>(self.children[in.a]->values[in.b]) ? 1 : 0; break;
        case Op::LoadMemberInt: (sp++)->i = std::get<std::int64_t>(self.children[in.a]->values[in.b]); break;
        case Op::LoadMemberReal: (sp++)->r = std::get<double>(self.children[in.a]->values[in.b]); break;
        case Op::StoreMemberBool: self.children[in.a]->values[in.b] = (--sp)->i != 0; break;
        case Op::StoreMemberInt16:
            self.children[in.a]->values[in.b] = std::int64_t{static_cast<std::int16_t>((--sp)->i)};
            break;
        case Op::StoreMemberInt32:
            self.children[in.a]->values[in.b] = std::int64_t{static_cast<std::int32_t>((--sp)->i)};
            break;
        case Op::StoreMemberReal: self.children[in.a]->values[in.b] = roundToReal((--sp)->r); break;

        case Op::LoadTemp: *sp++ = temps[in.a]; break;
        case Op::StoreTemp: temps[in.a] = *--sp; break;

        case Op::Index: {
            const auto& index = chunk.indices[in.a];
            const auto value = sp[-1].i;
            if (value < index.bounds.low || value > index.bounds.high) {
                throw RuntimeError{indexError(value, index.bounds), where()};
            }
            sp[-1].i = (value - index.bounds.low) * static_cast<std::int64_t>(index.stride);
            break;
        }
        case Op::LoadPlace: {
            const auto& info = chunk.places[in.a];
            const auto offset = (--sp)->i;
            *sp++ = toSlot(loadLeaf(vmPlace(info, self, offset), info.type->elementary, image_));
            break;
        }
        case Op::StorePlace: {
            const auto& info = chunk.places[in.a];
            const auto offset = (--sp)->i;
            const auto type = info.type->elementary;
            storeLeaf(vmPlace(info, self, offset), type, fromSlot(*--sp, type));
            break;
        }
        case Op::CopyPlace: {
            const auto& source = chunk.places[in.a];
            const auto& target = chunk.places[in.b];
            const auto targetOffset = (--sp)->i;
            const auto sourceOffset = (--sp)->i;
            copyAggregate(vmPlace(source, self, sourceOffset), vmPlace(target, self, targetOffset), *target.type);
            break;
        }

        case Op::AddI: --sp; sp[-1].i = add(sp[-1].i, sp->i); break;
        case Op::SubI: --sp; sp[-1].i = subtract(sp[-1].i, sp->i); break;
        case Op::MulI: --sp; sp[-1].i = multiply(sp[-1].i, sp->i); break;
        case Op::DivI:
        case Op::ModI:
            --sp;
            if (sp->i == 0) {
                throw RuntimeError{"division by zero", where()};
            }
            sp[-1].i = in.op == Op::DivI ? divide(sp[-1].i, sp->i) : modulo(sp[-1].i, sp->i);
            break;
        case Op::NegI: sp[-1].i = subtract(0, sp[-1].i); break;
        case Op::Wrap16: sp[-1].i = static_cast<std::int16_t>(sp[-1].i); break;
        case Op::Wrap32: sp[-1].i = static_cast<std::int32_t>(sp[-1].i); break;

        case Op::AddR: --sp; sp[-1].r = roundToReal(sp[-1].r + sp->r); break;
        case Op::SubR: --sp; sp[-1].r = roundToReal(sp[-1].r - sp->r); break;
        case Op::MulR: --sp; sp[-1].r = roundToReal(sp[-1].r * sp->r); break;
        case Op::DivR: --sp; sp[-1].r = roundToReal(sp[-1].r / sp->r); break;
        case Op::PowR: --sp; sp[-1].r = roundToReal(std::pow(sp[-1].r, sp->r)); break;
        case Op::NegR: sp[-1].r = -sp[-1].r; break;
        case Op::IntToReal: sp[-1].r = static_cast<double>(sp[-1].i); break;

        case Op::EqI: --sp; sp[-1].i = sp[-1].i == sp->i; break;
        case Op::NeI: --sp; sp[-1].i = sp[-1].i != sp->i; break;
        case Op::LtI: --sp; sp[-1].i = sp[-1].i < sp->i; break;
        case Op::LeI: --sp; sp[-1].i = sp[-1].i <= sp->i; break;
        case Op::GtI: --sp; sp[-1].i = sp[-1].i > sp->i; break;
        case Op::GeI: --sp; sp[-1].i = sp[-1].i >= sp->i; break;
        case Op::EqR: --sp; sp[-1].i = sp[-1].r == sp->r; break;
        case Op::NeR: --sp; sp[-1].i = sp[-1].r != sp->r; break;
        case Op::LtR: --sp; sp[-1].i = sp[-1].r < sp->r; break;
        case Op::LeR: --sp; sp[-1].i = sp[-1].r <= sp->r; break;
        case Op::GtR: --sp; sp[-1].i = sp[-1].r > sp->r; break;
        case Op::GeR: --sp; sp[-1].i = sp[-1].r >= sp->r; break;
        case Op::EqB: --sp; sp[-1].i = (sp[-1].i != 0) == (sp->i != 0); break;
        case Op::NeB: --sp; sp[-1].i = (sp[-1].i != 0) != (sp->i != 0); break;

        case Op::And: --sp; sp[-1].i = sp[-1].i != 0 && sp->i != 0; break;
        case Op::Or: --sp; sp[-1].i = sp[-1].i != 0 || sp->i != 0; break;
        case Op::Xor: --sp; sp[-1].i = (sp[-1].i != 0) != (sp->i != 0); break;
        case Op::Not: sp[-1].i = sp[-1].i == 0; break;

        case Op::Jump: pc = in.a; break;
        case Op::JumpIfFalse:
            if ((--sp)->i == 0) {
                pc = in.a;
            }
            break;
        case Op::JumpIfTrue:
            if ((--sp)->i != 0) {
                pc = in.a;
            }
            break;
        case Op::LoopTick: countLoopIteration(where()); break;
        case Op::ForTest: {
            const auto value = sp[-1].i;
            const auto to = temps[in.a].i;
            sp[-1].i = temps[in.b].i > 0 ? value <= to : value >= to;
            break;
        }
        case Op::CheckStep:
            if (temps[in.a].i == 0) {
                throw RuntimeError{"FOR loop with BY 0 would never end", where()};
            }
            break;
        case Op::Call: run(*self.children[in.a]); break;
        case Op::Return: return;
        }
    }
}

// A PlaceInfo plus the run-time part of its offset, as a Place.
Interpreter::Place Interpreter::vmPlace(const PlaceInfo& info, Instance& self, std::int64_t offset) const {
    Instance* instance = info.instanceSlot == kNoSlot ? &self : self.children[info.instanceSlot].get();
    const auto& variable = instance->type->variables[info.slot];
    const auto dynamic = static_cast<std::size_t>(offset);
    if (variable.address) {
        return Place{instance, &variable, 0, info.bitOffset + dynamic};
    }
    return Place{instance, &variable, info.leafOffset + dynamic, 0};
}

// ---------------------------------------------------------------- Expressions

Value Interpreter::evaluate(const Expression& expression, Instance& self) {
    return std::visit(Overloaded{
                          [](const Literal& literal) { return literal.value; },
                          [&](const Access& access) {
                              if (access.directLeaf != kNoSlot) {
                                  const Instance& owner =
                                      access.instanceSlot == kNoSlot ? self : *self.children[access.instanceSlot];
                                  return owner.values[access.directLeaf];
                              }
                              return loadLeaf(place(access, self), access.type->elementary, image_);
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
        return wrapInteger(binary.op == BinaryOperator::Divide ? divide(a, b) : modulo(a, b), type);
    default: break;
    }
    throw RuntimeError{fmt::format("operator '{}' is not supported here", operatorSymbol(binary.op)),
                       expression.location};
}

} // namespace softplc::st
