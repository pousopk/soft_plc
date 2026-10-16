#pragma once
#include <fstream>
#include <optional>
#include <sstream>
#include <string>

namespace softplc {

// Reads a whole file into a string, or nothing if it can't be opened.
inline std::optional<std::string> readFile(const std::string& path) {
    std::ifstream file(path);
    if (!file) {
        return std::nullopt;
    }
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

} // namespace softplc
