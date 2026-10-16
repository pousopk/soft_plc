#pragma once
#include "st/ast.hpp"
#include "st/diagnostic.hpp"
#include <string_view>
#include <tl/expected.hpp>
#include <vector>

namespace softplc::st {

// Parses a whole source file: any number of FUNCTION_BLOCKs and one PROGRAM.
//
//   FUNCTION_BLOCK Blinker
//   VAR_INPUT enable : BOOL; END_VAR
//   ...
//   END_FUNCTION_BLOCK
//
//   PROGRAM Main
//   VAR
//       count : INT := 0;
//       lamp AT %QX0.0 : BOOL;
//   END_VAR
//       count := count + 1;
//       lamp := count > 10;
//   END_PROGRAM
tl::expected<CompilationUnit, Diagnostic> parseProgram(std::string_view source);

// Parses a file that contains only FUNCTION_BLOCKs (used for the standard library).
tl::expected<std::vector<Pou>, Diagnostic> parseFunctionBlocks(std::string_view source);

// Parses statements on their own, without PROGRAM and VAR: `a := 1; IF a > 0 THEN b := TRUE; END_IF;`
tl::expected<StatementList, Diagnostic> parseStatements(std::string_view source);

// Parses a single expression with nothing after it: `b + 3 * c`
tl::expected<ExpressionPtr, Diagnostic> parseExpression(std::string_view source);

} // namespace softplc::st
