#pragma once
#include "st/ast.hpp"
#include "st/diagnostic.hpp"
#include <string_view>
#include <tl/expected.hpp>

namespace softplc::st {

// Parses a whole program:
//
//   PROGRAM Main
//   VAR
//       count : INT := 0;
//       lamp AT %QX0.0 : BOOL;
//   END_VAR
//       count := count + 1;
//       lamp := count > 10;
//   END_PROGRAM
tl::expected<ProgramUnit, Diagnostic> parseProgram(std::string_view source);

// Parses statements on their own, without PROGRAM and VAR: `a := 1; IF a > 0 THEN b := TRUE; END_IF;`
tl::expected<StatementList, Diagnostic> parseStatements(std::string_view source);

// Parses a single expression with nothing after it: `b + 3 * c`
tl::expected<ExpressionPtr, Diagnostic> parseExpression(std::string_view source);

} // namespace softplc::st
