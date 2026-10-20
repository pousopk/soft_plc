#pragma once
#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace softplc {

// ASCII upper-case copy. Structured Text names and keywords are
// case-insensitive, so they're compared in upper case.
inline std::string toUpper(std::string_view text) {
    std::string upper(text);
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](char c) { return static_cast<char>(std::toupper(static_cast<unsigned char>(c))); });
    return upper;
}

// Splits a monitor request or reply into words at spaces, keeping a text
// in quotes ('Hello World' or "a b") whole, also inside a word
// (msg='a b'). In quotes, $ escapes the next character, so $' doesn't end
// the text.
inline std::vector<std::string> splitQuotedWords(std::string_view text) {
    std::vector<std::string> words;
    std::string word;
    char quote = 0; // the quote we're inside, if any
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (quote == 0 && std::isspace(static_cast<unsigned char>(c)) != 0) {
            if (!word.empty()) {
                words.push_back(std::move(word));
                word.clear();
            }
            continue;
        }
        word += c;
        if (quote != 0 && c == '$' && i + 1 < text.size()) {
            word += text[++i];
        } else if (quote == 0 && (c == '\'' || c == '"')) {
            quote = c;
        } else if (c == quote) {
            quote = 0;
        }
    }
    if (!word.empty()) {
        words.push_back(std::move(word));
    }
    return words;
}

} // namespace softplc
