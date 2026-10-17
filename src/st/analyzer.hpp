#pragma once
#include "st/ast.hpp"
#include "st/diagnostic.hpp"
#include <cstddef>
#include <cstdint>
#include <map>
#include <string_view>
#include <tl/expected.hpp>

namespace softplc::st {

// Checks a parsed source file before it runs, like TIA Portal does when you
// compile. Each PROGRAM and FUNCTION_BLOCK is checked on its own, with its
// own variables:
//   - every variable is declared exactly once, and every use refers to one
//   - every type name is BOOL, INT, DINT, REAL, TIME, a TYPE or a known function block,
//     and no TYPE contains itself
//   - fields exist, arrays get the right number of integer indices, and
//     constant indices are inside the bounds
//   - whole arrays and structs are only assigned to the same type
//   - operators get operands of the right type (no BOOL + INT, no TIME + INT)
//   - conditions are BOOL, CASE selectors and FOR variables are integers
//   - assignments can't lose data (no DINT into INT)
//   - EXIT only appears inside a loop
//   - AT addresses match the variable's size (arrays and structs: an even
//     byte), and only appear in the PROGRAM
//   - initial values are constants of the right shape: [...] for arrays,
//     (field := ...) for structs
//   - calls only set inputs, and `t1.Q` only reads inputs and outputs
//   - no function block contains itself, directly or indirectly
//
// It also fills in what the interpreter needs: the type of every expression,
// the memory layout of every type, and where every access points (variable,
// offset and run-time indices).
tl::expected<void, Diagnostic> analyze(CompilationUnit& unit);

// Parses and analyzes in one step: the program is ready to run if this succeeds.
tl::expected<CompilationUnit, Diagnostic> compileProgram(std::string_view source);

// The data blocks the program's AT variables use, with the smallest size
// each one needs: `x AT %DB5.DBW10 : INT` needs DB5 with at least 12 bytes.
std::map<std::uint16_t, std::size_t> dataBlocksUsedBy(const CompilationUnit& unit);

} // namespace softplc::st
