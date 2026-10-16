#pragma once
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

    // Variables of the running instance (a = slot)
    LoadBool, LoadInt, LoadReal,
    StoreBool, StoreInt16, StoreInt32, StoreReal, // pop, convert to the type, store
    LoadAt, StoreAt,                              // AT variables, through the process image

    // Inputs and outputs of a child instance (a = instance slot, b = member slot)
    LoadMemberBool, LoadMemberInt, LoadMemberReal,
    StoreMemberBool, StoreMemberInt16, StoreMemberInt32, StoreMemberReal,

    // Scratch slots for the compiler's own use (CASE selector, FOR bounds), a = index
    LoadTemp, StoreTemp,

    // Integer arithmetic on 64 bits, then wrapped to the type's size
    AddI, SubI, MulI, DivI, ModI, NegI, Wrap16, Wrap32,
    // REAL arithmetic (rounded to 32-bit float precision)
    AddR, SubR, MulR, DivR, PowR, NegR,
    IntToReal, // convert the top of the stack from integer to REAL

    // Comparisons (push a BOOL)
    EqI, NeI, LtI, LeI, GtI, GeI,
    EqR, NeR, LtR, LeR, GtR, GeR,
    EqB, NeB,

    // Logic
    And, Or, Xor, Not,

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

// The compiled code of one POU.
struct Chunk {
    std::vector<Instruction> code;
    std::vector<SourceLocation> locations; // one per instruction, for runtime error messages
    std::size_t temps = 0;                 // scratch slots needed
    std::size_t maxStack = 0;              // deepest the operand stack ever gets
};

// A readable listing, one instruction per line:  "  12  AddI".
std::string disassemble(const Chunk& chunk);

} // namespace softplc::st
