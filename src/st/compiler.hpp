#pragma once
#include "st/ast.hpp"
#include "st/bytecode.hpp"

namespace softplc::st {

// Compiles the body of an analyzed POU into bytecode for the VM.
//
// Structured statements become jumps:
//
//   IF c THEN x; ELSE y; END_IF;        <c>
//                                       JumpIfFalse  ELSE
//                                       <x>
//                                       Jump         END
//                               ELSE:   <y>
//                               END:
//
//   WHILE c DO x; END_WHILE;     TOP:   <c>
//                                       JumpIfFalse  END
//                                       LoopTick
//                                       <x>
//                                       Jump         TOP
//                               END:
// `functions` are the FUNCTIONs of the same file, which calls refer to by index.
Chunk compile(const Pou& pou, const std::vector<Pou>& functions);

} // namespace softplc::st
