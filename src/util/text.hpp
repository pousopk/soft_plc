#pragma once
#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace softplc {

// ASCII upper-case copy. Structured Text names and keywords are
// case-insensitive, so they're compared in upper case.
inline std::string toUpper(std::string_view text) {
    std::string upper(text);
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](char c) { return static_cast<char>(std::toupper(static_cast<unsigned char>(c))); });
    return upper;
}

} // namespace softplc
