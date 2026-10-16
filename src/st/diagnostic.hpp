#pragma once
#include <cstddef>
#include <string>
#include <string_view>

namespace softplc::st {

// A position in the source text. Both start at 1, like in an editor.
struct SourceLocation {
    std::size_t line = 1;
    std::size_t column = 1;

    bool operator==(const SourceLocation&) const = default;
};

// An error found while reading Structured Text.
struct Diagnostic {
    std::string message;
    SourceLocation location;
};

// Formats a diagnostic like a compiler error, quoting the source line and
// putting a caret under the problem:
//
//   line 1, column 10: expected an expression, found '*'
//     a := b + * c;
//              ^
std::string formatDiagnostic(const Diagnostic& diagnostic, std::string_view source);

} // namespace softplc::st
