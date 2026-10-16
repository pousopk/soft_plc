#include "st/diagnostic.hpp"
#include <fmt/format.h>

namespace softplc::st {

namespace {

// Returns the text of line `lineNumber` (1-based), without its newline.
std::string_view lineAt(std::string_view source, std::size_t lineNumber) {
    for (std::size_t line = 1; line < lineNumber; ++line) {
        const auto newline = source.find('\n');
        if (newline == std::string_view::npos) {
            return {};
        }
        source.remove_prefix(newline + 1);
    }
    return source.substr(0, source.find('\n'));
}

} // namespace

std::string formatDiagnostic(const Diagnostic& diagnostic, std::string_view source) {
    const auto& [line, column] = diagnostic.location;
    const auto text = lineAt(source, line);

    // Copy tabs from the source line so the caret lines up in the terminal.
    std::string padding;
    for (std::size_t i = 0; i + 1 < column && i < text.size(); ++i) {
        padding += text[i] == '\t' ? '\t' : ' ';
    }

    return fmt::format("line {}, column {}: {}\n  {}\n  {}^", line, column, diagnostic.message, text, padding);
}

} // namespace softplc::st
