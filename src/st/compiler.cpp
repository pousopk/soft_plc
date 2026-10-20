#include "st/compiler.hpp"
#include "util/overloaded.hpp"
#include <bit>
#include <fmt/format.h>
#include <stdexcept>

namespace softplc::st {

namespace {

// How many values an instruction leaves on the stack, minus how many it takes.
int stackEffect(Op op) {
    switch (op) {
    case Op::PushInt: case Op::PushReal:
    case Op::LoadBool: case Op::LoadInt: case Op::LoadReal:
    case Op::LoadMemberBool: case Op::LoadMemberInt: case Op::LoadMemberReal:
    case Op::LoadTemp:
        return +1;
    case Op::StoreBool: case Op::StoreInt16: case Op::StoreInt32: case Op::StoreReal: case Op::StoreWrapped:
    case Op::StoreMemberBool: case Op::StoreMemberInt16: case Op::StoreMemberInt32: case Op::StoreMemberReal:
    case Op::StoreMemberWrapped:
    case Op::StoreTemp:
    case Op::AddI: case Op::SubI: case Op::MulI: case Op::DivI: case Op::ModI:
    case Op::AddR: case Op::SubR: case Op::MulR: case Op::DivR: case Op::PowR:
    case Op::EqI: case Op::NeI: case Op::LtI: case Op::LeI: case Op::GtI: case Op::GeI:
    case Op::EqR: case Op::NeR: case Op::LtR: case Op::LeR: case Op::GtR: case Op::GeR:
    case Op::EqB: case Op::NeB:
    case Op::And: case Op::Or: case Op::Xor:
    case Op::AndBits: case Op::OrBits: case Op::XorBits:
    case Op::JumpIfFalse: case Op::JumpIfTrue:
        return -1;
    case Op::StorePlace: case Op::StorePart: case Op::CopyPlace:
        return -2;
    default:
        return 0; // NegI, NegR, Wrap*, IntToReal, Not, NotBits, Jump, LoopTick, ForTest, CheckStep, Call, Return,
                  // Index, LoadPlace
    }
}

class Compiler {
public:
    Compiler(const Pou& pou, const std::vector<Pou>* functions) : pou_(pou), functions_(functions) {}

    Chunk run() {
        statements(pou_.body);
        emit(Op::Return, {});
        chunk_.maxStack = static_cast<std::size_t>(maxDepth_);
        return std::move(chunk_);
    }

private:
    // ------------------------------------------------------------ Emitting

    std::size_t emit(Op op, SourceLocation where, std::uint32_t a = 0, std::uint32_t b = 0, std::int64_t imm = 0) {
        chunk_.code.push_back(Instruction{op, a, b, imm});
        chunk_.locations.push_back(where);
        depth_ += stackEffect(op);
        maxDepth_ = std::max(maxDepth_, depth_);
        return chunk_.code.size() - 1;
    }

    std::size_t here() const { return chunk_.code.size(); }

    // Jumps are emitted before their target is known, then patched.
    void patchJump(std::size_t jump, std::size_t target) {
        chunk_.code[jump].a = static_cast<std::uint32_t>(target);
    }

    std::uint32_t newTemp() { return static_cast<std::uint32_t>(chunk_.temps++); }

    static std::uint32_t slotIndex(std::size_t slot) { return static_cast<std::uint32_t>(slot); }

    void wrapFor(DataType type, SourceLocation where) {
        if (type == DataType::Int) {
            emit(Op::Wrap16, where);
        } else if (type == DataType::DInt || type == DataType::Time) {
            emit(Op::Wrap32, where);
        } else if (type != DataType::LInt && type != DataType::AnyInt) {
            emit(Op::WrapTo, where, 0, 0, static_cast<std::int64_t>(type));
        } // LINT, and AnyInt (literals): plain 64-bit arithmetic, which already wraps
    }

    // An integer value where a REAL is needed (e.g. INT + REAL, REAL := INT).
    void convertIfNeeded(DataType from, DataType to, SourceLocation where) {
        if (to == DataType::Real && from != DataType::Real) {
            emit(Op::IntToReal, where);
        }
    }

    // ------------------------------------------------------------ Expressions

    void expression(const Expression& e) {
        std::visit(Overloaded{
                       [&](const Literal& literal) { this->literal(literal, e.location); },
                       [&](const Access& access) { load(access, e.location); },
                       [&](const CallExpression& call) { this->call(call, e.location); },
                       [&](const UnaryExpression& unary) {
                           expression(*unary.operand);
                           if (unary.op == UnaryOperator::Not && e.type == DataType::Bool) {
                               emit(Op::Not, e.location);
                           } else if (unary.op == UnaryOperator::Not) {
                               emit(Op::NotBits, e.location); // a bit string
                               wrapFor(e.type, e.location);
                           } else if (e.type == DataType::Real) {
                               emit(Op::NegR, e.location);
                           } else {
                               emit(Op::NegI, e.location);
                               wrapFor(e.type, e.location);
                           }
                       },
                       [&](const BinaryExpression& binary) { this->binary(binary, e); },
                   },
                   e.node);
    }

    void literal(const Literal& literal, SourceLocation where) {
        std::visit(Overloaded{
                       [&](bool b) { emit(Op::PushInt, where, 0, 0, b ? 1 : 0); },
                       [&](std::int64_t i) { emit(Op::PushInt, where, 0, 0, i); },
                       [&](double d) { emit(Op::PushReal, where, 0, 0, std::bit_cast<std::int64_t>(d)); },
                   },
                   literal.value);
    }

    // ------------------------------------------------------------ Accesses

    // A plain variable of this POU, as an Access (for FOR loop variables).
    Access variableAccess(std::size_t slot) const {
        Access access;
        access.name = pou_.variables[slot].name;
        access.slot = slot;
        access.type = pou_.variables[slot].typeInfo;
        return access;
    }

    const VariableDeclaration& variableOf(const Access& access) const {
        const Pou& owner = access.instanceSlot == kNoSlot ? pou_ : *pou_.variables[access.instanceSlot].functionBlock;
        return owner.variables[access.slot];
    }

    // Accesses with a fixed offset into memory that isn't in the process
    // image get the fast instructions; the rest use a place.
    bool isDirect(const Access& access) const {
        return access.dynamic.empty() && !variableOf(access).address && !access.part;
    }

    std::uint32_t addPart(const BitPart& part) {
        chunk_.parts.push_back(part);
        return static_cast<std::uint32_t>(chunk_.parts.size() - 1);
    }

    std::uint32_t addPlace(const Access& access) {
        chunk_.places.push_back(PlaceInfo{access.instanceSlot, access.slot, access.leafOffset, access.bitOffset, access.type});
        return static_cast<std::uint32_t>(chunk_.places.size() - 1);
    }

    // Pushes the run-time part of an access's offset: each index, checked
    // and multiplied by its stride, all added up (0 when there's none).
    void pushOffset(const Access& access, SourceLocation where) {
        const bool located = variableOf(access).address.has_value();
        if (access.dynamic.empty()) {
            emit(Op::PushInt, where, 0, 0, 0);
            return;
        }
        for (std::size_t i = 0; i < access.dynamic.size(); ++i) {
            const auto& term = access.dynamic[i];
            expression(*term.index);
            chunk_.indices.push_back(IndexInfo{term.bounds, located ? term.bitStride : term.leafStride});
            emit(Op::Index, term.index->location, static_cast<std::uint32_t>(chunk_.indices.size() - 1));
            if (i > 0) {
                emit(Op::AddI, where);
            }
        }
    }

    void load(const Access& access, SourceLocation where) {
        const auto& variable = variableOf(access);
        const auto type = access.type->elementary;
        if (!isDirect(access)) {
            pushOffset(access, where);
            if (access.part) {
                emit(Op::LoadPart, where, addPlace(access), addPart(*access.part));
            } else {
                emit(Op::LoadPlace, where, addPlace(access));
            }
            return;
        }
        const auto leaf = slotIndex(variable.leaf + access.leafOffset);
        if (access.instanceSlot != kNoSlot) {
            const Op op = type == DataType::Bool ? Op::LoadMemberBool : type == DataType::Real ? Op::LoadMemberReal
                                                                                               : Op::LoadMemberInt;
            emit(op, where, slotIndex(access.instanceSlot), leaf);
        } else if (type == DataType::Bool) {
            emit(Op::LoadBool, where, leaf);
        } else if (type == DataType::Real) {
            emit(Op::LoadReal, where, leaf);
        } else {
            emit(Op::LoadInt, where, leaf);
        }
    }

    // A function call: every argument onto the stack (converted to the type
    // it's passed as; arrays and structs as the offset of their place),
    // then one instruction that pops them all and pushes the result.
    void call(const CallExpression& call, SourceLocation where) {
        CallInfo info{&call, {}};
        const Pou* function = call.builtin == BuiltinFunction::None ? &unitFunction(call) : nullptr;
        for (std::size_t i = 0; i < call.arguments.size(); ++i) {
            const auto& argument = call.arguments[i];
            if (function == nullptr) {
                expression(*argument.value);
                convertIfNeeded(argument.value->type, call.argumentTypes[i], argument.location);
                info.sourcePlaces.push_back(CallInfo::kNoPlace);
                continue;
            }
            const auto& input = function->variables[argument.slot];
            if (input.typeInfo->isAggregate()) {
                const auto& source = std::get<Access>(argument.value->node);
                pushOffset(source, argument.location);
                info.sourcePlaces.push_back(addPlace(source));
            } else {
                expression(*argument.value);
                convertIfNeeded(argument.value->type, input.type, argument.location);
                info.sourcePlaces.push_back(CallInfo::kNoPlace);
            }
        }
        chunk_.calls.push_back(std::move(info));
        emit(function == nullptr ? Op::CallBuiltin : Op::CallFunction, where,
             static_cast<std::uint32_t>(chunk_.calls.size() - 1));
        depth_ += 1 - static_cast<int>(call.arguments.size()); // pops the arguments, pushes the result
    }

    const Pou& unitFunction(const CallExpression& call) const { return (*functions_)[call.functionIndex]; }

    DataType memberType(std::size_t instanceSlot, std::size_t memberSlot) const {
        return pou_.variables[instanceSlot].functionBlock->variables[memberSlot].type;
    }

    void binary(const BinaryExpression& b, const Expression& e) {
        const auto where = e.location;
        const auto type = b.operandType;

        if (b.op == BinaryOperator::And || b.op == BinaryOperator::Or || b.op == BinaryOperator::Xor) {
            expression(*b.left);
            expression(*b.right);
            if (type == DataType::Bool) {
                emit(b.op == BinaryOperator::And ? Op::And : b.op == BinaryOperator::Or ? Op::Or : Op::Xor, where);
            } else { // bit strings: a BOOL among them is 0 or 1 on the stack already
                emit(b.op == BinaryOperator::And ? Op::AndBits : b.op == BinaryOperator::Or ? Op::OrBits : Op::XorBits,
                     where);
            }
            return;
        }

        // Both operands are brought to the operation's type first.
        expression(*b.left);
        convertIfNeeded(b.left->type, type, where);
        expression(*b.right);
        convertIfNeeded(b.right->type, type, where);

        const bool real = type == DataType::Real;
        switch (b.op) {
        case BinaryOperator::Equal: emit(type == DataType::Bool ? Op::EqB : real ? Op::EqR : Op::EqI, where); return;
        case BinaryOperator::NotEqual: emit(type == DataType::Bool ? Op::NeB : real ? Op::NeR : Op::NeI, where); return;
        case BinaryOperator::Less: emit(real ? Op::LtR : Op::LtI, where); return;
        case BinaryOperator::LessEqual: emit(real ? Op::LeR : Op::LeI, where); return;
        case BinaryOperator::Greater: emit(real ? Op::GtR : Op::GtI, where); return;
        case BinaryOperator::GreaterEqual: emit(real ? Op::GeR : Op::GeI, where); return;
        case BinaryOperator::Power: emit(Op::PowR, where); return; // operandType is REAL for **
        default: break;
        }

        if (real) {
            switch (b.op) {
            case BinaryOperator::Add: emit(Op::AddR, where); return;
            case BinaryOperator::Subtract: emit(Op::SubR, where); return;
            case BinaryOperator::Multiply: emit(Op::MulR, where); return;
            case BinaryOperator::Divide: emit(Op::DivR, where); return;
            default: break;
            }
        }
        switch (b.op) {
        case BinaryOperator::Add: emit(Op::AddI, where); break;
        case BinaryOperator::Subtract: emit(Op::SubI, where); break;
        case BinaryOperator::Multiply: emit(Op::MulI, where); break;
        case BinaryOperator::Divide: emit(Op::DivI, where); break;
        case BinaryOperator::Modulo: emit(Op::ModI, where); break;
        default: throw std::logic_error("compiler: unexpected operator");
        }
        wrapFor(type, where);
    }

    // ------------------------------------------------------------ Storing

    // Pops the top of the stack into what `access` points to. The offset of
    // a place is computed here, after the value, like the tree-walker does.
    void store(const Access& access, SourceLocation where) {
        const auto& variable = variableOf(access);
        if (!isDirect(access)) {
            pushOffset(access, where);
            if (access.part) {
                emit(Op::StorePart, where, addPlace(access), addPart(*access.part));
            } else {
                emit(Op::StorePlace, where, addPlace(access));
            }
            return;
        }
        const auto leaf = slotIndex(variable.leaf + access.leafOffset);
        if (access.instanceSlot != kNoSlot) {
            storeMember(access.instanceSlot, leaf, access.type->elementary, where);
            return;
        }
        const auto type = access.type->elementary;
        switch (type) {
        case DataType::Bool: emit(Op::StoreBool, where, leaf); break;
        case DataType::Int: emit(Op::StoreInt16, where, leaf); break;
        case DataType::Real: emit(Op::StoreReal, where, leaf); break;
        case DataType::DInt:
        case DataType::Time: emit(Op::StoreInt32, where, leaf); break;
        default: emit(Op::StoreWrapped, where, leaf, 0, static_cast<std::int64_t>(type)); break;
        }
    }

    void storeMember(std::size_t instanceSlot, std::uint32_t leaf, DataType type, SourceLocation where) {
        const Op op = type == DataType::Bool                              ? Op::StoreMemberBool
                      : type == DataType::Int                             ? Op::StoreMemberInt16
                      : type == DataType::Real                            ? Op::StoreMemberReal
                      : type == DataType::DInt || type == DataType::Time ? Op::StoreMemberInt32
                                                                          : Op::StoreMemberWrapped;
        emit(op, where, slotIndex(instanceSlot), leaf, static_cast<std::int64_t>(type));
    }

    // target := source, for whole arrays and structs.
    void copy(const Access& source, const Access& target, SourceLocation where) {
        pushOffset(source, where);
        pushOffset(target, where);
        const auto from = addPlace(source);
        emit(Op::CopyPlace, where, from, addPlace(target));
    }

    // ------------------------------------------------------------ Statements

    void statements(const StatementList& list) {
        for (const auto& statement : list) {
            std::visit([&](const auto& node) { this->statement(node, statement.location); }, statement.node);
        }
    }

    void statement(const Assignment& s, SourceLocation where) {
        if (s.target.type->isAggregate()) {
            copy(std::get<Access>(s.value->node), s.target, where);
            return;
        }
        expression(*s.value);
        convertIfNeeded(s.value->type, s.target.type->elementary, where);
        store(s.target, where);
    }

    void statement(const CallStatement& s, SourceLocation where) {
        for (const auto& argument : s.arguments) {
            const auto& input = pou_.variables[s.instanceSlot].functionBlock->variables[argument.slot];
            if (input.typeInfo->isAggregate()) {
                Access target;
                target.name = input.name;
                target.instanceSlot = s.instanceSlot;
                target.slot = argument.slot;
                target.type = input.typeInfo;
                copy(std::get<Access>(argument.value->node), target, argument.location);
                continue;
            }
            expression(*argument.value);
            convertIfNeeded(argument.value->type, memberType(s.instanceSlot, argument.slot), argument.location);
            storeMember(s.instanceSlot, slotIndex(input.leaf), input.type, argument.location);
        }
        emit(Op::Call, where, slotIndex(s.instanceSlot));
    }

    void statement(const IfStatement& s, SourceLocation where) {
        std::vector<std::size_t> toEnd;
        for (const auto& branch : s.branches) {
            expression(*branch.condition);
            const auto skip = emit(Op::JumpIfFalse, where);
            statements(branch.body);
            toEnd.push_back(emit(Op::Jump, where));
            patchJump(skip, here()); // condition false: try the next branch
        }
        statements(s.elseBody);
        for (const auto jump : toEnd) {
            patchJump(jump, here());
        }
    }

    void statement(const CaseStatement& s, SourceLocation where) {
        const auto selector = newTemp();
        expression(*s.selector);
        emit(Op::StoreTemp, where, selector);

        // First all the label tests, each jumping to its branch's body...
        std::vector<std::vector<std::size_t>> toBody(s.branches.size());
        for (std::size_t i = 0; i < s.branches.size(); ++i) {
            for (const auto& range : s.branches[i].labels) {
                emit(Op::LoadTemp, where, selector);
                if (range.low == range.high) {
                    emit(Op::PushInt, where, 0, 0, range.low);
                    emit(Op::EqI, where);
                } else {
                    emit(Op::PushInt, where, 0, 0, range.low);
                    emit(Op::GeI, where);
                    emit(Op::LoadTemp, where, selector);
                    emit(Op::PushInt, where, 0, 0, range.high);
                    emit(Op::LeI, where);
                    emit(Op::And, where);
                }
                toBody[i].push_back(emit(Op::JumpIfTrue, where));
            }
        }
        // ...no label matched: the ELSE part.
        statements(s.elseBody);
        std::vector<std::size_t> toEnd{emit(Op::Jump, where)};

        // Then the bodies.
        for (std::size_t i = 0; i < s.branches.size(); ++i) {
            for (const auto jump : toBody[i]) {
                patchJump(jump, here());
            }
            statements(s.branches[i].body);
            toEnd.push_back(emit(Op::Jump, where));
        }
        for (const auto jump : toEnd) {
            patchJump(jump, here());
        }
    }

    void statement(const ForStatement& s, SourceLocation where) {
        const auto& variable = pou_.variables[s.slot];
        const auto loopVariable = variableAccess(s.slot);
        const auto to = newTemp();
        const auto step = newTemp();

        // Same order as the interpreter: TO and BY first, then the check, then FROM.
        expression(*s.to);
        emit(Op::StoreTemp, where, to);
        if (s.step) {
            expression(*s.step);
        } else {
            emit(Op::PushInt, where, 0, 0, 1);
        }
        emit(Op::StoreTemp, where, step);
        emit(Op::CheckStep, where, step);
        expression(*s.from);
        store(loopVariable, where);

        const auto top = here();
        load(loopVariable, where);
        emit(Op::ForTest, where, to, step);
        const auto exit = emit(Op::JumpIfFalse, where);
        emit(Op::LoopTick, where);

        exits_.emplace_back();
        statements(s.body);

        load(loopVariable, where);
        emit(Op::LoadTemp, where, step);
        emit(Op::AddI, where);
        wrapFor(variable.type, where);
        store(loopVariable, where);
        emit(Op::Jump, where, static_cast<std::uint32_t>(top));

        patchJump(exit, here());
        closeLoop();
    }

    void statement(const WhileStatement& s, SourceLocation where) {
        const auto top = here();
        expression(*s.condition);
        const auto exit = emit(Op::JumpIfFalse, where);
        emit(Op::LoopTick, where);
        exits_.emplace_back();
        statements(s.body);
        emit(Op::Jump, where, static_cast<std::uint32_t>(top));
        patchJump(exit, here());
        closeLoop();
    }

    void statement(const RepeatStatement& s, SourceLocation where) {
        const auto top = here();
        emit(Op::LoopTick, where);
        exits_.emplace_back();
        statements(s.body);
        expression(*s.condition);
        emit(Op::JumpIfFalse, where, static_cast<std::uint32_t>(top)); // UNTIL false: go round again
        closeLoop();
    }

    void statement(const ExitStatement&, SourceLocation where) {
        exits_.back().push_back(emit(Op::Jump, where)); // to the end of the innermost loop, patched later
    }

    void statement(const ReturnStatement&, SourceLocation where) { emit(Op::Return, where); }

    // Points every EXIT of the loop being closed at the instruction after it.
    void closeLoop() {
        for (const auto jump : exits_.back()) {
            patchJump(jump, here());
        }
        exits_.pop_back();
    }

    const Pou& pou_;
    const std::vector<Pou>* functions_; // the FUNCTIONs that calls refer to
    Chunk chunk_;
    int depth_ = 0;
    int maxDepth_ = 0;
    std::vector<std::vector<std::size_t>> exits_; // per enclosing loop: its EXIT jumps
};

} // namespace

Chunk compile(const Pou& pou, const std::vector<Pou>& functions) { return Compiler(pou, &functions).run(); }

const char* opName(Op op) {
    switch (op) {
    case Op::PushInt: return "PushInt";
    case Op::PushReal: return "PushReal";
    case Op::LoadBool: return "LoadBool";
    case Op::LoadInt: return "LoadInt";
    case Op::LoadReal: return "LoadReal";
    case Op::StoreBool: return "StoreBool";
    case Op::StoreInt16: return "StoreInt16";
    case Op::StoreInt32: return "StoreInt32";
    case Op::StoreReal: return "StoreReal";
    case Op::StoreWrapped: return "StoreWrapped";
    case Op::StoreMemberWrapped: return "StoreMemberWrapped";
    case Op::WrapTo: return "WrapTo";
    case Op::AndBits: return "AndBits";
    case Op::OrBits: return "OrBits";
    case Op::XorBits: return "XorBits";
    case Op::NotBits: return "NotBits";
    case Op::LoadPart: return "LoadPart";
    case Op::StorePart: return "StorePart";
    case Op::LoadMemberBool: return "LoadMemberBool";
    case Op::LoadMemberInt: return "LoadMemberInt";
    case Op::LoadMemberReal: return "LoadMemberReal";
    case Op::StoreMemberBool: return "StoreMemberBool";
    case Op::StoreMemberInt16: return "StoreMemberInt16";
    case Op::StoreMemberInt32: return "StoreMemberInt32";
    case Op::StoreMemberReal: return "StoreMemberReal";
    case Op::LoadTemp: return "LoadTemp";
    case Op::StoreTemp: return "StoreTemp";
    case Op::Index: return "Index";
    case Op::LoadPlace: return "LoadPlace";
    case Op::StorePlace: return "StorePlace";
    case Op::CopyPlace: return "CopyPlace";
    case Op::AddI: return "AddI";
    case Op::SubI: return "SubI";
    case Op::MulI: return "MulI";
    case Op::DivI: return "DivI";
    case Op::ModI: return "ModI";
    case Op::NegI: return "NegI";
    case Op::Wrap16: return "Wrap16";
    case Op::Wrap32: return "Wrap32";
    case Op::AddR: return "AddR";
    case Op::SubR: return "SubR";
    case Op::MulR: return "MulR";
    case Op::DivR: return "DivR";
    case Op::PowR: return "PowR";
    case Op::NegR: return "NegR";
    case Op::IntToReal: return "IntToReal";
    case Op::EqI: return "EqI";
    case Op::NeI: return "NeI";
    case Op::LtI: return "LtI";
    case Op::LeI: return "LeI";
    case Op::GtI: return "GtI";
    case Op::GeI: return "GeI";
    case Op::EqR: return "EqR";
    case Op::NeR: return "NeR";
    case Op::LtR: return "LtR";
    case Op::LeR: return "LeR";
    case Op::GtR: return "GtR";
    case Op::GeR: return "GeR";
    case Op::EqB: return "EqB";
    case Op::NeB: return "NeB";
    case Op::And: return "And";
    case Op::Or: return "Or";
    case Op::Xor: return "Xor";
    case Op::Not: return "Not";
    case Op::Jump: return "Jump";
    case Op::JumpIfFalse: return "JumpIfFalse";
    case Op::JumpIfTrue: return "JumpIfTrue";
    case Op::LoopTick: return "LoopTick";
    case Op::ForTest: return "ForTest";
    case Op::CheckStep: return "CheckStep";
    case Op::Call: return "Call";
    case Op::CallBuiltin: return "CallBuiltin";
    case Op::CallFunction: return "CallFunction";
    case Op::Return: return "Return";
    }
    return "?";
}

std::string disassemble(const Chunk& chunk) {
    std::string out = fmt::format("; {} instructions, {} temps, max stack {}\n", chunk.code.size(), chunk.temps,
                                  chunk.maxStack);
    for (std::size_t i = 0; i < chunk.code.size(); ++i) {
        const auto& in = chunk.code[i];
        std::string operands;
        switch (in.op) {
        case Op::PushInt: operands = fmt::format("{}", in.imm); break;
        case Op::PushReal: operands = fmt::format("{}", std::bit_cast<double>(in.imm)); break;
        case Op::LoadMemberBool: case Op::LoadMemberInt: case Op::LoadMemberReal:
        case Op::StoreMemberBool: case Op::StoreMemberInt16: case Op::StoreMemberInt32: case Op::StoreMemberReal:
        case Op::ForTest: case Op::CopyPlace: case Op::LoadPart: case Op::StorePart:
            operands = fmt::format("{} {}", in.a, in.b);
            break;
        case Op::StoreMemberWrapped:
            operands = fmt::format("{} {} {}", in.a, in.b, dataTypeName(static_cast<DataType>(in.imm)));
            break;
        case Op::StoreWrapped:
            operands = fmt::format("{} {}", in.a, dataTypeName(static_cast<DataType>(in.imm)));
            break;
        case Op::WrapTo: operands = dataTypeName(static_cast<DataType>(in.imm)); break;
        case Op::LoadBool: case Op::LoadInt: case Op::LoadReal: case Op::StoreBool: case Op::StoreInt16:
        case Op::StoreInt32: case Op::StoreReal: case Op::Index: case Op::LoadPlace: case Op::StorePlace: case Op::LoadTemp:
        case Op::StoreTemp: case Op::Jump: case Op::JumpIfFalse: case Op::JumpIfTrue: case Op::CheckStep:
        case Op::Call:
            operands = fmt::format("{}", in.a);
            break;
        case Op::CallBuiltin: case Op::CallFunction:
            operands = fmt::format("{}  ; {}", in.a, chunk.calls[in.a].call->name);
            break;
        default: break;
        }
        out += fmt::format("{:4}  {:<16} {}\n", i, opName(in.op), operands);
    }
    return out;
}

} // namespace softplc::st
