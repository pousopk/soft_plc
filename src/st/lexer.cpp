#include "st/lexer.hpp"
#include "util/text.hpp"
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
    case TokenKind::Time: return "TIME literal";
    case TokenKind::DirectAddress: return "address";
    case TokenKind::True: return "'TRUE'";
    case TokenKind::False: return "'FALSE'";
    case TokenKind::Assign: return "':='";
    case TokenKind::Semicolon: return "';'";
    case TokenKind::Colon: return "':'";
    case TokenKind::Comma: return "','";
    case TokenKind::DotDot: return "'..'";
    case TokenKind::Dot: return "'.'";
    case TokenKind::LeftParen: return "'('";
    case TokenKind::RightParen: return "')'";
    case TokenKind::LeftBracket: return "'['";
    case TokenKind::RightBracket: return "']'";
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
    case TokenKind::Program: return "'PROGRAM'";
    case TokenKind::EndProgram: return "'END_PROGRAM'";
    case TokenKind::FunctionBlock: return "'FUNCTION_BLOCK'";
    case TokenKind::EndFunctionBlock: return "'END_FUNCTION_BLOCK'";
    case TokenKind::Function: return "'FUNCTION'";
    case TokenKind::EndFunction: return "'END_FUNCTION'";
    case TokenKind::Var: return "'VAR'";
    case TokenKind::VarInput: return "'VAR_INPUT'";
    case TokenKind::VarOutput: return "'VAR_OUTPUT'";
    case TokenKind::EndVar: return "'END_VAR'";
    case TokenKind::At: return "'AT'";
    case TokenKind::Type: return "'TYPE'";
    case TokenKind::EndType: return "'END_TYPE'";
    case TokenKind::Struct: return "'STRUCT'";
    case TokenKind::EndStruct: return "'END_STRUCT'";
    case TokenKind::Array: return "'ARRAY'";
    case TokenKind::If: return "'IF'";
    case TokenKind::Then: return "'THEN'";
    case TokenKind::Elsif: return "'ELSIF'";
    case TokenKind::Else: return "'ELSE'";
    case TokenKind::EndIf: return "'END_IF'";
    case TokenKind::Case: return "'CASE'";
    case TokenKind::Of: return "'OF'";
    case TokenKind::EndCase: return "'END_CASE'";
    case TokenKind::For: return "'FOR'";
    case TokenKind::To: return "'TO'";
    case TokenKind::By: return "'BY'";
    case TokenKind::Do: return "'DO'";
    case TokenKind::EndFor: return "'END_FOR'";
    case TokenKind::While: return "'WHILE'";
    case TokenKind::EndWhile: return "'END_WHILE'";
    case TokenKind::Repeat: return "'REPEAT'";
    case TokenKind::Until: return "'UNTIL'";
    case TokenKind::EndRepeat: return "'END_REPEAT'";
    case TokenKind::Exit: return "'EXIT'";
    case TokenKind::Return: return "'RETURN'";
    case TokenKind::EndOfFile: return "end of input";
    }
    return "token";
}

namespace {

// Every keyword, in upper case. A name that isn't in this table is an identifier.
constexpr std::pair<std::string_view, TokenKind> keywords[] = {
    {"TRUE", TokenKind::True},       {"FALSE", TokenKind::False},
    {"MOD", TokenKind::Mod},         {"AND", TokenKind::And},
    {"OR", TokenKind::Or},           {"XOR", TokenKind::Xor},
    {"NOT", TokenKind::Not},         {"PROGRAM", TokenKind::Program},
    {"END_PROGRAM", TokenKind::EndProgram}, {"VAR", TokenKind::Var},
    {"END_VAR", TokenKind::EndVar},  {"AT", TokenKind::At},
    {"VAR_INPUT", TokenKind::VarInput}, {"VAR_OUTPUT", TokenKind::VarOutput},
    {"FUNCTION_BLOCK", TokenKind::FunctionBlock}, {"END_FUNCTION_BLOCK", TokenKind::EndFunctionBlock},
    {"FUNCTION", TokenKind::Function}, {"END_FUNCTION", TokenKind::EndFunction},
    {"IF", TokenKind::If},           {"THEN", TokenKind::Then},
    {"ELSIF", TokenKind::Elsif},     {"ELSE", TokenKind::Else},
    {"END_IF", TokenKind::EndIf},    {"CASE", TokenKind::Case},
    {"OF", TokenKind::Of},           {"END_CASE", TokenKind::EndCase},
    {"FOR", TokenKind::For},         {"TO", TokenKind::To},
    {"BY", TokenKind::By},           {"DO", TokenKind::Do},
    {"END_FOR", TokenKind::EndFor},  {"WHILE", TokenKind::While},
    {"END_WHILE", TokenKind::EndWhile}, {"REPEAT", TokenKind::Repeat},
    {"UNTIL", TokenKind::Until},     {"END_REPEAT", TokenKind::EndRepeat},
    {"EXIT", TokenKind::Exit},       {"RETURN", TokenKind::Return},
    {"TYPE", TokenKind::Type},       {"END_TYPE", TokenKind::EndType},
    {"STRUCT", TokenKind::Struct},   {"END_STRUCT", TokenKind::EndStruct},
    {"ARRAY", TokenKind::Array},
};

// Thrown inside the lexer to abort on the first error; tokenize() catches it
// and turns it into the tl::expected error. Callers never see the exception.
struct LexError {
    Diagnostic diagnostic;
};

bool isDigit(char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }
bool isIdentifierStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_'; }
bool isIdentifierChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; }

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
        if (c == '%') {
            return directAddress();
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
        if ((upper == "T" || upper == "TIME") && peek() == '#') {
            return timeLiteral(start, begin);
        }

        auto kind = TokenKind::Identifier;
        for (const auto& [keyword, keywordKind] : keywords) {
            if (upper == keyword) {
                kind = keywordKind;
                break;
            }
        }
        return Token{kind, std::move(text), start, {}};
    }

    // T#1h2m3s4ms or TIME#500ms: one or more <number><unit> parts, with units
    // d, h, m, s and ms (any case, largest first). The value is in milliseconds.
    Token timeLiteral(SourceLocation start, std::size_t begin) {
        advance(); // '#'
        std::int64_t total = 0;
        bool anyPart = false;
        while (isDigit(peek())) {
            const auto numberBegin = pos_;
            readDigits(10);
            const auto digits = withoutUnderscores(source_.substr(numberBegin, pos_ - numberBegin));
            std::int64_t amount = 0;
            std::from_chars(digits.data(), digits.data() + digits.size(), amount);

            const auto unitBegin = pos_;
            while (std::isalpha(static_cast<unsigned char>(peek())) != 0) {
                advance();
            }
            const auto unit = toUpper(source_.substr(unitBegin, pos_ - unitBegin));
            std::int64_t millisecondsPerUnit = 0;
            if (unit == "D") millisecondsPerUnit = 86'400'000;
            else if (unit == "H") millisecondsPerUnit = 3'600'000;
            else if (unit == "M") millisecondsPerUnit = 60'000;
            else if (unit == "S") millisecondsPerUnit = 1'000;
            else if (unit == "MS") millisecondsPerUnit = 1;
            else {
                fail(start, fmt::format("unknown TIME unit '{}' (use d, h, m, s or ms)",
                                        source_.substr(unitBegin, pos_ - unitBegin)));
            }
            total += amount * millisecondsPerUnit;
            anyPart = true;
        }

        std::string text(source_.substr(begin, pos_ - begin));
        if (!anyPart || isIdentifierChar(peek()) || peek() == '.') {
            fail(start, fmt::format("invalid TIME literal '{}' (examples: T#500ms, T#1m30s)", text));
        }
        return Token{TokenKind::Time, std::move(text), start, total};
    }

    // %IX0.0, %QB1, %MW10 ... validated with the same parser as the rest of the PLC.
    Token directAddress() {
        const auto start = location_;
        const auto begin = pos_;
        advance(); // '%'
        while (isIdentifierChar(peek()) || peek() == '.') {
            advance();
        }
        std::string text(source_.substr(begin, pos_ - begin));
        auto address = parseAddress(text);
        if (!address) {
            fail(start, address.error());
        }
        return Token{TokenKind::DirectAddress, std::move(text), start, *address};
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
        case ',': return make(TokenKind::Comma, ",");
        case '.':
            if (peek() == '.') {
                advance();
                return make(TokenKind::DotDot, "..");
            }
            return make(TokenKind::Dot, ".");
        case '(': return make(TokenKind::LeftParen, "(");
        case ')': return make(TokenKind::RightParen, ")");
        case '[': return make(TokenKind::LeftBracket, "[");
        case ']': return make(TokenKind::RightBracket, "]");
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
            return make(TokenKind::Colon, ":");
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
