#pragma once
#include "st/ast.hpp"
#include "st/diagnostic.hpp"
#include <string_view>
#include <tl/expected.hpp>
#include <vector>

namespace softplc::st {

// Parses a sequence of assignments: `a := b + 3 * c; done := a > 10;`
tl::expected<std::vector<Assignment>, Diagnostic> parseStatements(std::string_view source);

// Parses a single expression with nothing after it: `b + 3 * c`
tl::expected<ExpressionPtr, Diagnostic> parseExpression(std::string_view source);

} // namespace softplc::st
