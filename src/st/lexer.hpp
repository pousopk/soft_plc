#pragma once
#include "st/diagnostic.hpp"
#include "st/token.hpp"
#include <string_view>
#include <tl/expected.hpp>
#include <vector>

namespace softplc::st {

// Splits Structured Text source into tokens, skipping whitespace and
// comments (both (* block *) and // line comments). Keywords are
// case-insensitive, as in IEC 61131-3: TRUE, true and True are the same.
//
// On success, the last token is always EndOfFile. On failure, returns the
// first problem found.
tl::expected<std::vector<Token>, Diagnostic> tokenize(std::string_view source);

} // namespace softplc::st
