#pragma once
#include "st/ast.hpp"
#include "st/diagnostic.hpp"
#include <string_view>
#include <tl/expected.hpp>

namespace softplc::st {

// Checks a parsed source file before it runs, like TIA Portal does when you
// compile. Each PROGRAM and FUNCTION_BLOCK is checked on its own, with its
// own variables:
//   - every variable is declared exactly once, and every use refers to one
//   - every type name is BOOL, INT, DINT, REAL, TIME or a known function block
//   - operators get operands of the right type (no BOOL + INT, no TIME + INT)
//   - conditions are BOOL, CASE selectors and FOR variables are integers
//   - assignments can't lose data (no DINT into INT)
//   - EXIT only appears inside a loop
//   - AT addresses match the variable's size, and only appear in the PROGRAM
//   - calls only set inputs, and `t1.Q` only reads inputs and outputs
//   - no function block contains itself, directly or indirectly
//
// It also fills in what the interpreter needs: the type of every expression
// and the slot of every variable, member and call argument.
tl::expected<void, Diagnostic> analyze(CompilationUnit& unit);

// Parses and analyzes in one step: the program is ready to run if this succeeds.
tl::expected<CompilationUnit, Diagnostic> compileProgram(std::string_view source);

} // namespace softplc::st
