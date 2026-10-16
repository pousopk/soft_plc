#include "st/parser.hpp"
#include "st/lexer.hpp"
#include <fmt/format.h>
#include <initializer_list>
#include <utility>

namespace softplc::st {

namespace {

// Thrown inside the parser to abort on the first error; the public functions
// catch it and turn it into the tl::expected error.
struct ParseError {
    Diagnostic diagnostic;
};

ExpressionPtr makeExpression(decltype(Expression::node) node, SourceLocation location) {
    return std::make_unique<Expression>(Expression{std::move(node), location});
}

// A recursive descent parser: one function per precedence level, from the
// loosest-binding operator (OR) down to the tightest (parentheses).
// Each level parses its operands by calling the next, tighter level, which is
// what makes `a + b * c` group as `a + (b * c)`.
//
// Precedence, IEC 61131-3 (3rd edition), tightest first:
//   ( )  literals  variables      primary()
//   -  +  NOT   (unary)           unary()
//   **                            power()
//   *  /  MOD                     multiplicative()
//   +  -                          additive()
//   <  >  <=  >=                  comparison()
//   =  <>                         equality()
//   AND  &                        andExpression()
//   XOR                           xorExpression()
//   OR                            orExpression()
class Parser {
public:
    explicit Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

    std::vector<Assignment> statements() {
        std::vector<Assignment> result;
        while (!check(TokenKind::EndOfFile)) {
            result.push_back(assignment());
        }
        return result;
    }

    ExpressionPtr expressionOnly() {
        auto result = expression();
        if (!check(TokenKind::EndOfFile)) {
            fail(peek(), fmt::format("unexpected {} after the expression", describe(peek())));
        }
        return result;
    }

private:
    using Level = ExpressionPtr (Parser::*)(); // pointer to one of the level functions below

    Assignment assignment() {
        const Token& target = expect(TokenKind::Identifier, "a variable name at the start of the statement");
        expect(TokenKind::Assign, fmt::format("':=' after '{}'", target.text));
        auto value = expression();
        expect(TokenKind::Semicolon, "';' at the end of the statement");
        return Assignment{target.text, std::move(value), target.location};
    }

    ExpressionPtr expression() { return orExpression(); }

    ExpressionPtr orExpression() { return binaryLevel(&Parser::xorExpression, {{TokenKind::Or, BinaryOperator::Or}}); }

    ExpressionPtr xorExpression() {
        return binaryLevel(&Parser::andExpression, {{TokenKind::Xor, BinaryOperator::Xor}});
    }

    ExpressionPtr andExpression() { return binaryLevel(&Parser::equality, {{TokenKind::And, BinaryOperator::And}}); }

    ExpressionPtr equality() {
        return binaryLevel(&Parser::comparison,
                           {{TokenKind::Equal, BinaryOperator::Equal}, {TokenKind::NotEqual, BinaryOperator::NotEqual}});
    }

    ExpressionPtr comparison() {
        return binaryLevel(&Parser::additive, {{TokenKind::Less, BinaryOperator::Less},
                                               {TokenKind::LessEqual, BinaryOperator::LessEqual},
                                               {TokenKind::Greater, BinaryOperator::Greater},
                                               {TokenKind::GreaterEqual, BinaryOperator::GreaterEqual}});
    }

    ExpressionPtr additive() {
        return binaryLevel(&Parser::multiplicative,
                           {{TokenKind::Plus, BinaryOperator::Add}, {TokenKind::Minus, BinaryOperator::Subtract}});
    }

    ExpressionPtr multiplicative() {
        return binaryLevel(&Parser::power, {{TokenKind::Star, BinaryOperator::Multiply},
                                            {TokenKind::Slash, BinaryOperator::Divide},
                                            {TokenKind::Mod, BinaryOperator::Modulo}});
    }

    ExpressionPtr power() { return binaryLevel(&Parser::unary, {{TokenKind::Power, BinaryOperator::Power}}); }

    // Shared by every binary level: parse an operand with the next level, then
    // keep consuming `operator operand` pairs while the operator belongs to
    // this level. Building the tree as we go makes operators left-associative:
    // a - b - c  ->  ((a - b) - c)
    ExpressionPtr binaryLevel(Level next, std::initializer_list<std::pair<TokenKind, BinaryOperator>> operators) {
        auto left = (this->*next)();
        for (;;) {
            const auto* match = findOperator(operators);
            if (match == nullptr) {
                return left;
            }
            const Token& opToken = advance();
            auto right = (this->*next)();
            left = makeExpression(BinaryExpression{match->second, std::move(left), std::move(right)}, opToken.location);
        }
    }

    const std::pair<TokenKind, BinaryOperator>*
    findOperator(std::initializer_list<std::pair<TokenKind, BinaryOperator>> operators) const {
        for (const auto& entry : operators) {
            if (check(entry.first)) {
                return &entry;
            }
        }
        return nullptr;
    }

    ExpressionPtr unary() {
        if (check(TokenKind::Minus) || check(TokenKind::Not)) {
            const Token& opToken = advance();
            const auto op = opToken.kind == TokenKind::Minus ? UnaryOperator::Negate : UnaryOperator::Not;
            return makeExpression(UnaryExpression{op, unary()}, opToken.location);
        }
        if (check(TokenKind::Plus)) {
            advance(); // unary plus changes nothing
            return unary();
        }
        return primary();
    }

    ExpressionPtr primary() {
        const Token& token = peek();
        switch (token.kind) {
        case TokenKind::Integer:
            advance();
            return makeExpression(Literal{std::get<std::int64_t>(token.value)}, token.location);
        case TokenKind::Real:
            advance();
            return makeExpression(Literal{std::get<double>(token.value)}, token.location);
        case TokenKind::True:
        case TokenKind::False:
            advance();
            return makeExpression(Literal{token.kind == TokenKind::True}, token.location);
        case TokenKind::Identifier:
            advance();
            return makeExpression(VariableRef{token.text}, token.location);
        case TokenKind::LeftParen: {
            advance();
            auto inner = expression();
            expect(TokenKind::RightParen, "')' to close the '('");
            return inner;
        }
        default: fail(token, fmt::format("expected an expression, found {}", describe(token)));
        }
    }

    // --- Token helpers ---

    const Token& peek() const { return tokens_[pos_]; }

    bool check(TokenKind kind) const { return peek().kind == kind; }

    const Token& advance() {
        const Token& token = tokens_[pos_];
        if (token.kind != TokenKind::EndOfFile) {
            ++pos_; // never move past EndOfFile
        }
        return token;
    }

    const Token& expect(TokenKind kind, const std::string& what) {
        if (!check(kind)) {
            fail(peek(), fmt::format("expected {}, found {}", what, describe(peek())));
        }
        return advance();
    }

    static std::string describe(const Token& token) {
        if (token.kind == TokenKind::EndOfFile) {
            return "end of input";
        }
        return fmt::format("'{}'", token.text);
    }

    [[noreturn]] static void fail(const Token& at, std::string message) {
        throw ParseError{Diagnostic{std::move(message), at.location}};
    }

    std::vector<Token> tokens_;
    std::size_t pos_ = 0;
};

} // namespace

tl::expected<std::vector<Assignment>, Diagnostic> parseStatements(std::string_view source) {
    auto tokens = tokenize(source);
    if (!tokens) {
        return tl::unexpected(tokens.error());
    }
    try {
        return Parser(std::move(*tokens)).statements();
    } catch (const ParseError& error) {
        return tl::unexpected(error.diagnostic);
    }
}

tl::expected<ExpressionPtr, Diagnostic> parseExpression(std::string_view source) {
    auto tokens = tokenize(source);
    if (!tokens) {
        return tl::unexpected(tokens.error());
    }
    try {
        return Parser(std::move(*tokens)).expressionOnly();
    } catch (const ParseError& error) {
        return tl::unexpected(error.diagnostic);
    }
}

} // namespace softplc::st
