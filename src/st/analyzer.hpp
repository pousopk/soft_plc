#pragma once
#include "st/ast.hpp"
#include "st/diagnostic.hpp"
#include <string_view>
#include <tl/expected.hpp>

namespace softplc::st {

// Checks a parsed program before it runs, like TIA Portal does when you
// compile:
//   - every variable is declared exactly once, and every use refers to one
//   - operators get operands of the right type (no BOOL + INT)
//   - conditions are BOOL, CASE selectors and FOR variables are integers
//   - assignments can't lose data (no DINT into INT)
//   - EXIT only appears inside a loop
//   - AT addresses match the variable's size (a BOOL needs a bit address)
//
// It also fills in what the interpreter needs: the type of every expression
// and the storage slot of every variable reference.
tl::expected<void, Diagnostic> analyze(ProgramUnit& program);

// Parses and analyzes in one step: the program is ready to run if this succeeds.
tl::expected<ProgramUnit, Diagnostic> compileProgram(std::string_view source);

} // namespace softplc::st
