#include "st/lexer.hpp"
#include <algorithm>
#include <cctype>
#include <charconv>
#include <fmt/format.h>

namespace softplc::st {

const char* tokenKindName(TokenKind kind) {
    switch (kind) {
    case TokenKind::Identifier: return "identifier";
    case TokenKind::Integer: return "integer";
    case TokenKind::Real: return "real number";
    case TokenKind::True: return "'TRUE'";
    case TokenKind::False: return "'FALSE'";
    case TokenKind::Assign: return "':='";
    case TokenKind::Semicolon: return "';'";
    case TokenKind::LeftParen: return "'('";
    case TokenKind::RightParen: return "')'";
    case TokenKind::Plus: return "'+'";
    case TokenKind::Minus: return "'-'";
    case TokenKind::Star: return "'*'";
    case TokenKind::Slash: return "'/'";
    case TokenKind::Power: return "'**'";
    case TokenKind::Mod: return "'MOD'";
    case TokenKind::And: return "'AND'";
    case TokenKind::Or: return "'OR'";
    case TokenKind::Xor: return "'XOR'";
    case TokenKind::Not: return "'NOT'";
    case TokenKind::Equal: return "'='";
    case TokenKind::NotEqual: return "'<>'";
    case TokenKind::Less: return "'<'";
    case TokenKind::LessEqual: return "'<='";
    case TokenKind::Greater: return "'>'";
    case TokenKind::GreaterEqual: return "'>='";
    case TokenKind::EndOfFile: return "end of input";
    }
    return "token";
}

namespace {

// Thrown inside the lexer to abort on the first error; tokenize() catches it
// and turns it into the tl::expected error. Callers never see the exception.
struct LexError {
    Diagnostic diagnostic;
};

bool isDigit(char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }
bool isIdentifierStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_'; }
bool isIdentifierChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; }

std::string toUpper(std::string_view text) {
    std::string upper(text);
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](char c) { return static_cast<char>(std::toupper(static_cast<unsigned char>(c))); });
    return upper;
}

std::string withoutUnderscores(std::string_view text) {
    std::string result;
    std::copy_if(text.begin(), text.end(), std::back_inserter(result), [](char c) { return c != '_'; });
    return result;
}

class Lexer {
public:
    explicit Lexer(std::string_view source) : source_(source) {}

    std::vector<Token> run() {
        std::vector<Token> tokens;
        for (;;) {
            skipWhitespaceAndComments();
            if (atEnd()) {
                tokens.push_back(Token{TokenKind::EndOfFile, "", location_, {}});
                return tokens;
            }
            tokens.push_back(nextToken());
        }
    }

private:
    bool atEnd() const { return pos_ >= source_.size(); }

    char peek(std::size_t ahead = 0) const {
        return pos_ + ahead < source_.size() ? source_[pos_ + ahead] : '\0';
    }

    // Consumes one character, keeping line and column up to date.
    char advance() {
        const char c = source_[pos_++];
        if (c == '\n') {
            ++location_.line;
            location_.column = 1;
        } else {
            ++location_.column;
        }
        return c;
    }

    [[noreturn]] void fail(SourceLocation where, std::string message) const {
        throw LexError{Diagnostic{std::move(message), where}};
    }

    void skipWhitespaceAndComments() {
        while (!atEnd()) {
            if (std::isspace(static_cast<unsigned char>(peek())) != 0) {
                advance();
            } else if (peek() == '/' && peek(1) == '/') {
                while (!atEnd() && peek() != '\n') {
                    advance();
                }
            } else if (peek() == '(' && peek(1) == '*') {
                const auto start = location_;
                advance();
                advance();
                while (!(peek() == '*' && peek(1) == ')')) {
                    if (atEnd()) {
                        fail(start, "comment is never closed with '*)'");
                    }
                    advance();
                }
                advance();
                advance();
            } else {
                return;
            }
        }
    }

    Token nextToken() {
        const char c = peek();
        if (isDigit(c)) {
            return number();
        }
        if (isIdentifierStart(c)) {
            return identifierOrKeyword();
        }
        return symbol();
    }

    Token identifierOrKeyword() {
        const auto start = location_;
        const auto begin = pos_;
        while (isIdentifierChar(peek())) {
            advance();
        }
        std::string text(source_.substr(begin, pos_ - begin));

        const auto upper = toUpper(text);
        auto kind = TokenKind::Identifier;
        if (upper == "TRUE") kind = TokenKind::True;
        else if (upper == "FALSE") kind = TokenKind::False;
        else if (upper == "MOD") kind = TokenKind::Mod;
        else if (upper == "AND") kind = TokenKind::And;
        else if (upper == "OR") kind = TokenKind::Or;
        else if (upper == "XOR") kind = TokenKind::Xor;
        else if (upper == "NOT") kind = TokenKind::Not;

        return Token{kind, std::move(text), start, {}};
    }

    // Reads 42, 1_000, 16#FF, 2#1010_0101, 3.14 or 1.5E-3.
    Token number() {
        const auto start = location_;
        const auto begin = pos_;
        readDigits(10);

        Token token;
        if (peek() == '#') {
            token = basedInteger(start, begin);
        } else if (peek() == '.' && isDigit(peek(1))) {
            token = real(start, begin);
        } else {
            token = decimalInteger(start, begin);
        }

        if (isIdentifierChar(peek())) {
            fail(start, fmt::format("invalid number '{}{}'", token.text, peek()));
        }
        return token;
    }

    // Consumes digits valid in `base`, plus '_' separators.
    void readDigits(int base) {
        for (;;) {
            const char c = peek();
            const bool valid = c == '_' || (base == 10 && isDigit(c)) ||
                               (base == 2 && (c == '0' || c == '1')) || (base == 8 && c >= '0' && c <= '7') ||
                               (base == 16 && std::isxdigit(static_cast<unsigned char>(c)) != 0);
            if (!valid) {
                return;
            }
            advance();
        }
    }

    Token decimalInteger(SourceLocation start, std::size_t begin) {
        std::string text(source_.substr(begin, pos_ - begin));
        return Token{TokenKind::Integer, text, start, parseInteger(start, text, withoutUnderscores(text), 10)};
    }

    // Base#digits, e.g. 16#FF. Only bases 2, 8 and 16 exist in IEC 61131-3.
    Token basedInteger(SourceLocation start, std::size_t begin) {
        const auto baseText = withoutUnderscores(source_.substr(begin, pos_ - begin));
        if (baseText != "2" && baseText != "8" && baseText != "16") {
            fail(start, fmt::format("base must be 2, 8 or 16, not {}", baseText));
        }
        const int base = std::stoi(baseText);

        advance(); // '#'
        const auto digitsBegin = pos_;
        readDigits(base);
        const auto digits = withoutUnderscores(source_.substr(digitsBegin, pos_ - digitsBegin));
        std::string text(source_.substr(begin, pos_ - begin));
        if (digits.empty()) {
            fail(start, fmt::format("'{}' needs base-{} digits after '#'", text, base));
        }
        return Token{TokenKind::Integer, text, start, parseInteger(start, text, digits, base)};
    }

    std::int64_t parseInteger(SourceLocation start, const std::string& text, const std::string& digits, int base) {
        std::int64_t value = 0;
        const auto [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), value, base);
        if (ec == std::errc::result_out_of_range) {
            fail(start, fmt::format("integer '{}' is too large", text));
        }
        if (ec != std::errc{} || ptr != digits.data() + digits.size()) {
            fail(start, fmt::format("invalid integer '{}'", text));
        }
        return value;
    }

    Token real(SourceLocation start, std::size_t begin) {
        advance(); // '.'
        readDigits(10);

        // Optional exponent: E or e, an optional sign, then digits.
        const bool hasExponent = (peek() == 'E' || peek() == 'e') &&
                                 (isDigit(peek(1)) || ((peek(1) == '+' || peek(1) == '-') && isDigit(peek(2))));
        if (hasExponent) {
            advance();
            if (peek() == '+' || peek() == '-') {
                advance();
            }
            readDigits(10);
        }

        std::string text(source_.substr(begin, pos_ - begin));
        const auto digits = withoutUnderscores(text);
        double value = 0.0;
        const auto [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), value);
        if (ec != std::errc{} || ptr != digits.data() + digits.size()) {
            fail(start, fmt::format("invalid real number '{}'", text));
        }
        return Token{TokenKind::Real, std::move(text), start, value};
    }

    Token symbol() {
        const auto start = location_;
        const char c = advance();
        const auto make = [&](TokenKind kind, std::string text) { return Token{kind, std::move(text), start, {}}; };

        switch (c) {
        case ';': return make(TokenKind::Semicolon, ";");
        case '(': return make(TokenKind::LeftParen, "(");
        case ')': return make(TokenKind::RightParen, ")");
        case '+': return make(TokenKind::Plus, "+");
        case '-': return make(TokenKind::Minus, "-");
        case '/': return make(TokenKind::Slash, "/");
        case '&': return make(TokenKind::And, "&");
        case '=': return make(TokenKind::Equal, "=");
        case '*':
            if (peek() == '*') {
                advance();
                return make(TokenKind::Power, "**");
            }
            return make(TokenKind::Star, "*");
        case ':':
            if (peek() == '=') {
                advance();
                return make(TokenKind::Assign, ":=");
            }
            fail(start, "unexpected ':' (did you mean ':='?)");
        case '<':
            if (peek() == '=') {
                advance();
                return make(TokenKind::LessEqual, "<=");
            }
            if (peek() == '>') {
                advance();
                return make(TokenKind::NotEqual, "<>");
            }
            return make(TokenKind::Less, "<");
        case '>':
            if (peek() == '=') {
                advance();
                return make(TokenKind::GreaterEqual, ">=");
            }
            return make(TokenKind::Greater, ">");
        default: fail(start, fmt::format("unexpected character '{}'", c));
        }
    }

    std::string_view source_;
    std::size_t pos_ = 0;
    SourceLocation location_;
};

} // namespace

tl::expected<std::vector<Token>, Diagnostic> tokenize(std::string_view source) {
    try {
        return Lexer(source).run();
    } catch (const LexError& error) {
        return tl::unexpected(error.diagnostic);
    }
}

} // namespace softplc::st
