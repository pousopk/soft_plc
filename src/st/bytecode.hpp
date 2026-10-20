#pragma once
#include "st/ast.hpp"
#include "st/diagnostic.hpp"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace softplc::st {

// The instruction set of the bytecode VM: a stack machine. Most instructions
// take their operands from the top of the stack and push their result:
//
//   a := b + 3 * c;        LoadInt    b          stack: b
//                          PushInt    3          stack: b 3
//                          LoadInt    c          stack: b 3 c
//                          MulI                  stack: b (3*c)
//                          Wrap16                (INT arithmetic wraps at 16 bits)
//                          AddI                  stack: (b+3*c)
//                          Wrap16
//                          StoreInt16 a          stack: (empty)
//
// Instructions are typed (AddI for integers, AddR for REALs, ...), so the VM
// never has to check what kind of value it's looking at: the analyzer already
// worked all types out, and the compiler picks the right instruction.
enum class Op : std::uint8_t {
    // Constants
    PushInt,  // push imm (also BOOL 0/1 and TIME in ms)
    PushReal, // push imm, holding a double's bits

    // Values of the running instance (a = leaf)
    LoadBool, LoadInt, LoadReal,
    StoreBool, StoreInt16, StoreInt32, StoreReal, // pop, convert to the type, store
    StoreWrapped, // pop, wrap to the type in imm (a DataType: SINT, UINT, WORD, LINT, ...), store

    // Inputs and outputs of a child instance (a = instance slot, b = leaf in the child)
    LoadMemberBool, LoadMemberInt, LoadMemberReal,
    StoreMemberBool, StoreMemberInt16, StoreMemberInt32, StoreMemberReal,
    StoreMemberWrapped, // like StoreWrapped

    // Scratch slots for the compiler's own use (CASE selector, FOR bounds), a = index
    LoadTemp, StoreTemp,

    // Everything else: AT variables and array elements with run-time
    // indices. a = an entry in Chunk::places; the offset on the stack is
    // added to the place's fixed offset.
    Index,      // pop an array index, check it (a = entry in Chunk::indices), push its offset
    LoadPlace,  // pop the offset, push the value
    StorePlace, // pop the offset, pop the value, store it
    CopyPlace,  // pop the target's offset, then the source's; copy a whole array or struct (a = source, b = target)
    LoadPart,   // like LoadPlace, then take the part b (an entry in Chunk::parts), as in myWord.%X3
    StorePart,  // like StorePlace, but only into the part b: read the whole value, change the part, write it back

    // Function calls (a = entry in Chunk::calls): pop the arguments, push the result
    CallBuiltin,  // a standard function like MAX or REAL_TO_INT
    CallFunction, // a FUNCTION; its array and struct arguments come as offsets of their places

    // Integer arithmetic on 64 bits, then wrapped to the type's size
    AddI, SubI, MulI, DivI, ModI, NegI, Wrap16, Wrap32,
    WrapTo, // wrap to the type in imm, for the types without their own Wrap (SINT, UINT, WORD, ...)
    // REAL arithmetic (rounded to 32-bit float precision)
    AddR, SubR, MulR, DivR, PowR, NegR,
    IntToReal, // convert the top of the stack from integer to REAL

    // Comparisons (push a BOOL)
    EqI, NeI, LtI, LeI, GtI, GeI,
    EqR, NeR, LtR, LeR, GtR, GeR,
    EqB, NeB,

    // Logic
    And, Or, Xor, Not,
    // Bit strings, bit by bit (NotBits is followed by a WrapTo to the width)
    AndBits, OrBits, XorBits, NotBits,

    // Control flow (a = target instruction index)
    Jump, JumpIfFalse, JumpIfTrue,
    LoopTick,  // count one loop iteration for the watchdog
    ForTest,   // pop the loop variable; push "keep looping?" (a = temp with TO, b = temp with BY)
    CheckStep, // runtime error if the FOR step (temp a) is 0
    Call,      // run the child instance in slot a
    Return,
};

const char* opName(Op op);

struct Instruction {
    Op op;
    std::uint32_t a = 0; // a slot, temp or jump target: never negative
    std::uint32_t b = 0;
    std::int64_t imm = 0;
};

// A place the place instructions use: a variable, an offset inside it, and
// the type found there.
struct PlaceInfo {
    std::size_t instanceSlot = kNoSlot; // a child instance's member, or kNoSlot for the POU's own variable
    std::size_t slot = 0;               // the variable
    std::size_t leafOffset = 0;         // the fixed offset (leaves, or bits for AT variables)
    std::size_t bitOffset = 0;
    const Type* type = nullptr;
};

// One array index to check: its bounds, and what one step is worth.
struct IndexInfo {
    ArrayDimension bounds;
    std::size_t stride = 0; // in leaves, or in bits for AT variables
};

// A function call: the call in the syntax tree, and for each array or
// struct argument the place it's copied from (kNoPlace for the others).
struct CallInfo {
    static constexpr std::uint32_t kNoPlace = 0xFFFFFFFF;
    const CallExpression* call = nullptr;
    std::vector<std::uint32_t> sourcePlaces;
};

// The compiled code of one POU.
struct Chunk {
    std::vector<Instruction> code;
    std::vector<SourceLocation> locations; // one per instruction, for runtime error messages
    std::vector<PlaceInfo> places;
    std::vector<IndexInfo> indices;
    std::vector<CallInfo> calls;
    std::vector<BitPart> parts;            // for LoadPart and StorePart
    std::size_t temps = 0;                 // scratch slots needed
    std::size_t maxStack = 0;              // deepest the operand stack ever gets
};

// A readable listing, one instruction per line:  "  12  AddI".
std::string disassemble(const Chunk& chunk);

} // namespace softplc::st
