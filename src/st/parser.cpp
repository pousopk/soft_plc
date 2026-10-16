#include "st/parser.hpp"
#include "st/lexer.hpp"
#include "util/text.hpp"
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

// Keywords that close or split a block of statements.
constexpr std::initializer_list<TokenKind> blockEnds = {
    TokenKind::EndIf,    TokenKind::Elsif,  TokenKind::Else,      TokenKind::EndCase,    TokenKind::EndFor,
    TokenKind::EndWhile, TokenKind::Until,  TokenKind::EndRepeat, TokenKind::EndProgram, TokenKind::EndVar,
};

ExpressionPtr makeExpression(decltype(Expression::node) node, SourceLocation location) {
    return std::make_unique<Expression>(Expression{std::move(node), location});
}

// A recursive descent parser: one function per grammar rule.
//
// Expressions use one function per precedence level, from the loosest-binding
// operator (OR) down to the tightest (parentheses). Each level parses its
// operands by calling the next, tighter level, which is what makes
// `a + b * c` group as `a + (b * c)`.
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

    ProgramUnit program() {
        ProgramUnit unit;
        expect(TokenKind::Program, "'PROGRAM' at the start");
        unit.name = expect(TokenKind::Identifier, "the program name after PROGRAM").text;
        while (check(TokenKind::Var)) {
            varBlock(unit.variables);
        }
        unit.body = statementList({});
        expect(TokenKind::EndProgram, "'END_PROGRAM' at the end of the program");
        if (!check(TokenKind::EndOfFile)) {
            fail(peek(), fmt::format("unexpected {} after END_PROGRAM", describe(peek())));
        }
        return unit;
    }

    StatementList statementsOnly() {
        auto list = statementList({});
        if (!check(TokenKind::EndOfFile)) {
            fail(peek(), fmt::format("expected a statement, found {}", describe(peek())));
        }
        return list;
    }

    ExpressionPtr expressionOnly() {
        auto result = expression();
        if (!check(TokenKind::EndOfFile)) {
            fail(peek(), fmt::format("unexpected {} after the expression", describe(peek())));
        }
        return result;
    }

private:
    // ------------------------------------------------------------ Declarations

    // VAR  name, name AT %addr : TYPE := value;  ...  END_VAR
    void varBlock(std::vector<VariableDeclaration>& variables) {
        advance(); // VAR
        while (!check(TokenKind::EndVar)) {
            if (!check(TokenKind::Identifier)) {
                fail(peek(), fmt::format("expected a variable name or 'END_VAR', found {}", describe(peek())));
            }
            declarationLine(variables);
        }
        advance(); // END_VAR
    }

    void declarationLine(std::vector<VariableDeclaration>& variables) {
        std::vector<const Token*> names{&advance()};
        while (check(TokenKind::Comma)) {
            advance();
            names.push_back(&expect(TokenKind::Identifier, "a variable name after ','"));
        }

        std::optional<Address> address;
        if (check(TokenKind::At)) {
            const Token& at = advance();
            if (names.size() > 1) {
                fail(at, "AT can only be used with a single variable");
            }
            address = std::get<Address>(expect(TokenKind::DirectAddress, "an address like %IX0.0 after AT").value);
        }

        expect(TokenKind::Colon, "':' before the type");
        const DataType type = typeName();

        ExpressionPtr initialValue;
        if (check(TokenKind::Assign)) {
            advance();
            initialValue = expression();
        }
        expect(TokenKind::Semicolon, "';' at the end of the declaration");

        // `a, b : INT := 5;` declares two variables, each with its own copy of the initial value.
        for (const Token* name : names) {
            variables.push_back(VariableDeclaration{name->text, type, address,
                                                    initialValue ? cloneExpression(*initialValue) : nullptr,
                                                    name->location});
        }
    }

    DataType typeName() {
        const Token& token = expect(TokenKind::Identifier, "a type name");
        const auto upper = toUpper(token.text);
        if (upper == "BOOL") return DataType::Bool;
        if (upper == "INT") return DataType::Int;
        if (upper == "DINT") return DataType::DInt;
        if (upper == "REAL") return DataType::Real;
        fail(token, fmt::format("unknown type '{}' (supported: BOOL, INT, DINT, REAL)", token.text));
    }

    // ------------------------------------------------------------ Statements

    // Parses statements until a keyword that ends a block (END_IF, ELSE,
    // UNTIL, ...), one of the `extraTerminators`, or the end of input. The
    // caller then checks for the keyword it expects, which gives good messages
    // like "expected 'END_IF' to close the IF, found 'END_WHILE'".
    StatementList statementList(std::initializer_list<TokenKind> extraTerminators) {
        StatementList list;
        while (!check(TokenKind::EndOfFile) && !checkAny(blockEnds) && !checkAny(extraTerminators)) {
            if (check(TokenKind::Semicolon)) {
                advance(); // an empty statement is allowed
                continue;
            }
            list.push_back(statement());
        }
        return list;
    }

    Statement statement() {
        switch (peek().kind) {
        case TokenKind::Identifier: return assignment();
        case TokenKind::If: return ifStatement();
        case TokenKind::Case: return caseStatement();
        case TokenKind::For: return forStatement();
        case TokenKind::While: return whileStatement();
        case TokenKind::Repeat: return repeatStatement();
        case TokenKind::Exit: {
            const auto location = advance().location;
            expect(TokenKind::Semicolon, "';' after EXIT");
            return Statement{ExitStatement{}, location};
        }
        case TokenKind::Return: {
            const auto location = advance().location;
            expect(TokenKind::Semicolon, "';' after RETURN");
            return Statement{ReturnStatement{}, location};
        }
        default: fail(peek(), fmt::format("expected a statement, found {}", describe(peek())));
        }
    }

    // name := expression;
    Statement assignment() {
        const Token& target = advance();
        expect(TokenKind::Assign, fmt::format("':=' after '{}'", target.text));
        auto value = expression();
        expect(TokenKind::Semicolon, "';' at the end of the statement");
        return Statement{Assignment{target.text, std::move(value)}, target.location};
    }

    // IF c THEN ... ELSIF c THEN ... ELSE ... END_IF;
    Statement ifStatement() {
        const auto location = advance().location;
        IfStatement s;
        do {
            auto condition = expression();
            expect(TokenKind::Then, "'THEN' after the condition");
            auto body = statementList({});
            s.branches.push_back(IfBranch{std::move(condition), std::move(body)});
        } while (match(TokenKind::Elsif));

        if (match(TokenKind::Else)) {
            s.elseBody = statementList({});
        }
        expect(TokenKind::EndIf, "'END_IF' to close the IF");
        expect(TokenKind::Semicolon, "';' after END_IF");
        return Statement{std::move(s), location};
    }

    // CASE x OF 1: ... 2, 4..6: ... ELSE ... END_CASE;
    Statement caseStatement() {
        const auto location = advance().location;
        CaseStatement s;
        s.selector = expression();
        expect(TokenKind::Of, "'OF' after the CASE selector");

        // A statement never starts with a number or '-', so either one means a new label.
        while (check(TokenKind::Integer) || check(TokenKind::Minus)) {
            CaseBranch branch;
            branch.labels.push_back(caseRange());
            while (match(TokenKind::Comma)) {
                branch.labels.push_back(caseRange());
            }
            expect(TokenKind::Colon, "':' after the case label");
            branch.body = statementList({TokenKind::Integer, TokenKind::Minus});
            s.branches.push_back(std::move(branch));
        }
        if (s.branches.empty()) {
            fail(peek(), fmt::format("expected a case label like '1:', found {}", describe(peek())));
        }

        if (match(TokenKind::Else)) {
            s.elseBody = statementList({});
        }
        expect(TokenKind::EndCase, "'END_CASE' to close the CASE");
        expect(TokenKind::Semicolon, "';' after END_CASE");
        return Statement{std::move(s), location};
    }

    CaseRange caseRange() {
        const Token& first = peek();
        const auto low = caseLabelValue();
        auto high = low;
        if (match(TokenKind::DotDot)) {
            high = caseLabelValue();
            if (high < low) {
                fail(first, fmt::format("range {}..{} is empty: the first value must be the smaller one", low, high));
            }
        }
        return CaseRange{low, high};
    }

    std::int64_t caseLabelValue() {
        const bool negative = match(TokenKind::Minus);
        const auto value = std::get<std::int64_t>(expect(TokenKind::Integer, "an integer case label").value);
        return negative ? -value : value;
    }

    // FOR i := from TO to BY step DO ... END_FOR;
    Statement forStatement() {
        const auto location = advance().location;
        ForStatement s;
        s.variable = expect(TokenKind::Identifier, "the loop variable after FOR").text;
        expect(TokenKind::Assign, "':=' after the loop variable");
        s.from = expression();
        expect(TokenKind::To, "'TO' in the FOR loop");
        s.to = expression();
        if (match(TokenKind::By)) {
            s.step = expression();
        }
        expect(TokenKind::Do, "'DO' in the FOR loop");
        s.body = statementList({});
        expect(TokenKind::EndFor, "'END_FOR' to close the FOR");
        expect(TokenKind::Semicolon, "';' after END_FOR");
        return Statement{std::move(s), location};
    }

    // WHILE c DO ... END_WHILE;
    Statement whileStatement() {
        const auto location = advance().location;
        WhileStatement s;
        s.condition = expression();
        expect(TokenKind::Do, "'DO' after the WHILE condition");
        s.body = statementList({});
        expect(TokenKind::EndWhile, "'END_WHILE' to close the WHILE");
        expect(TokenKind::Semicolon, "';' after END_WHILE");
        return Statement{std::move(s), location};
    }

    // REPEAT ... UNTIL c END_REPEAT;
    Statement repeatStatement() {
        const auto location = advance().location;
        RepeatStatement s;
        s.body = statementList({});
        expect(TokenKind::Until, "'UNTIL' in the REPEAT loop");
        s.condition = expression();
        expect(TokenKind::EndRepeat, "'END_REPEAT' to close the REPEAT");
        expect(TokenKind::Semicolon, "';' after END_REPEAT");
        return Statement{std::move(s), location};
    }

    // ------------------------------------------------------------ Expressions

    using Level = ExpressionPtr (Parser::*)(); // pointer to one of the level functions below

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
            const auto* found = findOperator(operators);
            if (found == nullptr) {
                return left;
            }
            const Token& opToken = advance();
            auto right = (this->*next)();
            left = makeExpression(BinaryExpression{found->second, std::move(left), std::move(right)}, opToken.location);
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
        if (match(TokenKind::Plus)) {
            return unary(); // unary plus changes nothing
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

    // ------------------------------------------------------------ Token helpers

    const Token& peek() const { return tokens_[pos_]; }

    bool check(TokenKind kind) const { return peek().kind == kind; }

    bool checkAny(std::initializer_list<TokenKind> kinds) const {
        for (const auto kind : kinds) {
            if (check(kind)) {
                return true;
            }
        }
        return false;
    }

    const Token& advance() {
        const Token& token = tokens_[pos_];
        if (token.kind != TokenKind::EndOfFile) {
            ++pos_; // never move past EndOfFile
        }
        return token;
    }

    // Consumes the next token if it's of `kind`.
    bool match(TokenKind kind) {
        if (!check(kind)) {
            return false;
        }
        advance();
        return true;
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

// Shared by the public functions: tokenize, run one parser rule, and turn
// any error into a tl::expected error.
template <class Rule> auto parseWith(std::string_view source, Rule rule) -> tl::expected<decltype(rule(std::declval<Parser&>())), Diagnostic> {
    auto tokens = tokenize(source);
    if (!tokens) {
        return tl::unexpected(tokens.error());
    }
    try {
        Parser parser(std::move(*tokens));
        return rule(parser);
    } catch (const ParseError& error) {
        return tl::unexpected(error.diagnostic);
    }
}

} // namespace

tl::expected<ProgramUnit, Diagnostic> parseProgram(std::string_view source) {
    return parseWith(source, [](Parser& parser) { return parser.program(); });
}

tl::expected<StatementList, Diagnostic> parseStatements(std::string_view source) {
    return parseWith(source, [](Parser& parser) { return parser.statementsOnly(); });
}

tl::expected<ExpressionPtr, Diagnostic> parseExpression(std::string_view source) {
    return parseWith(source, [](Parser& parser) { return parser.expressionOnly(); });
}

} // namespace softplc::st
