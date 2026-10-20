#include "st/parser.hpp"
#include "st/lexer.hpp"
#include "util/text.hpp"
#include <algorithm>
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

// Keywords that close or split a block of statements, plus the ones that
// start a new POU (which mean the current one wasn't closed).
constexpr std::initializer_list<TokenKind> blockEnds = {
    TokenKind::EndIf,    TokenKind::Elsif,  TokenKind::Else,      TokenKind::EndCase,    TokenKind::EndFor,
    TokenKind::EndWhile, TokenKind::Until,  TokenKind::EndRepeat, TokenKind::EndProgram, TokenKind::EndVar,
    TokenKind::EndFunctionBlock, TokenKind::Program, TokenKind::FunctionBlock, TokenKind::Type,
    TokenKind::EndType, TokenKind::EndStruct, TokenKind::Function, TokenKind::EndFunction,
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

    // A whole file: TYPE blocks, any number of FUNCTION_BLOCKs and exactly
    // one PROGRAM, in any order.
    CompilationUnit compilationUnit() {
        CompilationUnit unit;
        bool haveProgram = false;
        while (!check(TokenKind::EndOfFile)) {
            if (check(TokenKind::Type)) {
                typeBlock(unit.types);
            } else if (check(TokenKind::Function)) {
                unit.functions.push_back(pou(PouKind::Function));
            } else if (check(TokenKind::FunctionBlock)) {
                unit.functionBlocks.push_back(pou(PouKind::FunctionBlock));
            } else if (check(TokenKind::Program)) {
                if (haveProgram) {
                    fail(peek(), "a file can only have one PROGRAM");
                }
                unit.program = pou(PouKind::Program);
                haveProgram = true;
            } else {
                fail(peek(), fmt::format("expected 'PROGRAM', 'FUNCTION_BLOCK', 'FUNCTION' or 'TYPE', found {}", describe(peek())));
            }
        }
        if (!haveProgram) {
            fail(peek(), "expected a PROGRAM");
        }
        return unit;
    }

    // Only FUNCTION_BLOCKs, used for the built-in standard library.
    std::vector<Pou> functionBlocksOnly() {
        std::vector<Pou> blocks;
        while (!check(TokenKind::EndOfFile)) {
            if (!check(TokenKind::FunctionBlock)) {
                fail(peek(), fmt::format("expected 'FUNCTION_BLOCK', found {}", describe(peek())));
            }
            blocks.push_back(pou(PouKind::FunctionBlock));
        }
        return blocks;
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

    // PROGRAM Name ... END_PROGRAM, FUNCTION_BLOCK Name ... END_FUNCTION_BLOCK
    // or FUNCTION Name : Type ... END_FUNCTION
    Pou pou(PouKind kind) {
        Pou unit;
        unit.kind = kind;
        unit.location = advance().location; // PROGRAM, FUNCTION_BLOCK or FUNCTION
        const char* what = kind == PouKind::Program         ? "the program name after PROGRAM"
                           : kind == PouKind::FunctionBlock ? "the function block name after FUNCTION_BLOCK"
                                                            : "the function name after FUNCTION";
        const Token& name = expect(TokenKind::Identifier, what);
        unit.name = name.text;
        if (kind == PouKind::Function) {
            expect(TokenKind::Colon, fmt::format("':' and the type the function returns, as in FUNCTION {} : REAL",
                                                 name.text));
            unit.returnType = typeSpec();
        }
        while (checkAny({TokenKind::Var, TokenKind::VarInput, TokenKind::VarOutput})) {
            varBlock(unit.variables);
        }
        unit.body = statementList({});
        switch (kind) {
        case PouKind::Program: expect(TokenKind::EndProgram, "'END_PROGRAM' at the end of the program"); break;
        case PouKind::FunctionBlock:
            expect(TokenKind::EndFunctionBlock, "'END_FUNCTION_BLOCK' at the end of the function block");
            break;
        case PouKind::Function: {
            expect(TokenKind::EndFunction, "'END_FUNCTION' at the end of the function");
            // The result is a variable named like the function: `Scale := ...;` sets it.
            VariableDeclaration result;
            result.name = unit.name;
            result.typeSpec = unit.returnType;
            if (unit.returnType.dimensions.empty()) {
                result.type = elementaryTypeFromName(unit.returnType.name).value_or(DataType::Unknown);
            }
            result.section = VarSection::Output;
            result.location = name.location;
            unit.returnSlot = unit.variables.size();
            unit.variables.push_back(std::move(result));
            break;
        }
        }
        return unit;
    }

    // VAR / VAR_INPUT / VAR_OUTPUT  name, name AT %addr : TYPE := value;  ...  END_VAR
    void varBlock(std::vector<VariableDeclaration>& variables) {
        const auto opener = advance().kind;
        const auto section = opener == TokenKind::VarInput    ? VarSection::Input
                             : opener == TokenKind::VarOutput ? VarSection::Output
                                                              : VarSection::Local;
        while (!check(TokenKind::EndVar)) {
            if (!check(TokenKind::Identifier)) {
                fail(peek(), fmt::format("expected a variable name or 'END_VAR', found {}", describe(peek())));
            }
            declarationLine(variables, section);
        }
        advance(); // END_VAR
    }

    // TYPE  Name : STRUCT ... END_STRUCT;  Name : ARRAY[1..3] OF INT;  ...  END_TYPE
    void typeBlock(std::vector<TypeDeclaration>& types) {
        advance(); // TYPE
        while (!check(TokenKind::EndType)) {
            TypeDeclaration type;
            const Token& name = expect(TokenKind::Identifier, "a type name or 'END_TYPE'");
            type.name = name.text;
            type.location = name.location;
            expect(TokenKind::Colon, fmt::format("':' after the type name '{}'", name.text));
            if (match(TokenKind::Struct)) {
                type.isStruct = true;
                while (!check(TokenKind::EndStruct)) {
                    if (!check(TokenKind::Identifier)) {
                        fail(peek(), fmt::format("expected a field name or 'END_STRUCT', found {}", describe(peek())));
                    }
                    declarationLine(type.fields, VarSection::Local);
                }
                advance(); // END_STRUCT
                if (type.fields.empty()) {
                    fail(name, fmt::format("STRUCT '{}' has no fields", name.text));
                }
            } else {
                type.alias = typeSpec();
            }
            expect(TokenKind::Semicolon, "';' after the type declaration");
            types.push_back(std::move(type));
        }
        advance(); // END_TYPE
    }

    void declarationLine(std::vector<VariableDeclaration>& variables, VarSection section) {
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
        const auto spec = typeSpec();

        InitializerPtr initialValue;
        if (check(TokenKind::Assign)) {
            advance();
            initialValue = initializer();
        }
        expect(TokenKind::Semicolon, "';' at the end of the declaration");

        // `a, b : INT := 5;` declares two variables, each with its own copy of the initial value.
        for (const Token* name : names) {
            VariableDeclaration declaration;
            declaration.name = name->text;
            declaration.typeSpec = spec;
            if (spec.dimensions.empty()) {
                declaration.type = elementaryTypeFromName(spec.name).value_or(DataType::Unknown);
            }
            declaration.section = section;
            declaration.address = address;
            declaration.initialValue = initialValue ? cloneInitializer(*initialValue) : nullptr;
            declaration.location = name->location;
            variables.push_back(std::move(declaration));
        }
    }

    // A type name (BOOL, INT, Point, TON, ...) or ARRAY[1..10, 0..3] OF name.
    // Whether the name exists is the analyzer's job.
    TypeSpec typeSpec() {
        TypeSpec spec;
        spec.location = peek().location;
        if (match(TokenKind::Array)) {
            expect(TokenKind::LeftBracket, "'[' after ARRAY, as in ARRAY[1..10] OF INT");
            do {
                const Token& first = peek();
                ArrayDimension dimension;
                dimension.low = signedInteger("the lower bound of the array, as in [1..10]");
                expect(TokenKind::DotDot, "'..' between the array bounds, as in [1..10]");
                dimension.high = signedInteger("the upper bound of the array, as in [1..10]");
                if (dimension.high < dimension.low) {
                    fail(first, fmt::format("array bounds {}..{} are empty: the first must be the smaller one",
                                            dimension.low, dimension.high));
                }
                spec.dimensions.push_back(dimension);
            } while (match(TokenKind::Comma));
            expect(TokenKind::RightBracket, "']' after the array bounds");
            expect(TokenKind::Of, "'OF' after ARRAY[...]");
            if (check(TokenKind::Array)) {
                fail(peek(), "an array of arrays isn't allowed: use several dimensions, as in ARRAY[1..3, 1..4] OF INT, "
                             "or declare the inner array as a TYPE");
            }
        }
        spec.name = expect(TokenKind::Identifier, "a type name").text;
        if (match(TokenKind::LeftBracket)) { // STRING[20]; the analyzer checks the type takes a length
            spec.stringLength = signedInteger("the string's length, as in STRING[20]");
            expect(TokenKind::RightBracket, "']' after the length");
        }
        return spec;
    }

    std::int64_t signedInteger(const std::string& what) {
        const bool negative = match(TokenKind::Minus);
        const auto value = std::get<std::int64_t>(expect(TokenKind::Integer, what).value);
        return negative ? -value : value;
    }

    // An initial value: an expression, [array items] or (field := value, ...).
    InitializerPtr initializer() {
        const auto location = peek().location;
        if (match(TokenKind::LeftBracket)) {
            ArrayInitializer array;
            do {
                array.items.push_back(arrayInitItem());
            } while (match(TokenKind::Comma));
            expect(TokenKind::RightBracket, "']' to close the array's initial values");
            return std::make_unique<Initializer>(Initializer{std::move(array), location});
        }
        if (check(TokenKind::LeftParen) && peekAt(1).kind == TokenKind::Identifier &&
            peekAt(2).kind == TokenKind::Assign) {
            advance(); // '('
            StructInitializer structure;
            do {
                const Token& name = expect(TokenKind::Identifier, "a field name");
                expect(TokenKind::Assign, fmt::format("':=' after '{}'", name.text));
                structure.fields.push_back(StructInitField{name.text, initializer(), name.location});
            } while (match(TokenKind::Comma));
            expect(TokenKind::RightParen, "')' to close the struct's initial values");
            return std::make_unique<Initializer>(Initializer{std::move(structure), location});
        }
        return std::make_unique<Initializer>(Initializer{expression(), location});
    }

    // 5, or 3(0) for "three times 0", or 3() for "three default values".
    ArrayInitItem arrayInitItem() {
        if (check(TokenKind::Integer) && peekAt(1).kind == TokenKind::LeftParen) {
            const Token& count = advance();
            const auto repeat = std::get<std::int64_t>(count.value);
            if (repeat < 1) {
                fail(count, "a repeat count must be at least 1");
            }
            advance(); // '('
            ArrayInitItem item{repeat, nullptr};
            if (!check(TokenKind::RightParen)) {
                item.value = initializer();
            }
            expect(TokenKind::RightParen, "')' after the repeated value");
            return item;
        }
        return ArrayInitItem{1, initializer()};
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
        case TokenKind::Identifier:
            if (peekNext().kind == TokenKind::LeftParen) {
                return call();
            }
            return assignment();
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

    // target := expression;   where target is a variable or a part of one: a, p.x, values[i]
    Statement assignment() {
        const auto location = peek().location;
        auto target = access();
        expect(TokenKind::Assign, fmt::format("':=' after '{}'", formatAccess(target)));
        auto value = expression();
        expect(TokenKind::Semicolon, "';' at the end of the statement");
        return Statement{Assignment{std::move(target), std::move(value)}, location};
    }

    // name, then any number of .field and [index, ...]
    Access access() {
        Access result;
        result.name = expect(TokenKind::Identifier, "a variable name").text;
        for (;;) {
            if (check(TokenKind::Dot) && peekNext().kind == TokenKind::PartSelector) {
                advance(); // '.'
                const Token& part = advance();
                const auto address = std::get<Address>(part.value);
                result.selectors.emplace_back(
                    PartSelector{address.size, static_cast<unsigned>(address.byteOffset), part.location});
            } else if (check(TokenKind::Dot)) {
                advance();
                const Token& field = expect(TokenKind::Identifier, fmt::format("a name after '{}.'", formatAccess(result)));
                result.selectors.emplace_back(FieldSelector{field.text, field.location});
            } else if (check(TokenKind::LeftBracket)) {
                IndexSelector index;
                index.location = advance().location;
                do {
                    index.indices.push_back(expression());
                } while (match(TokenKind::Comma));
                expect(TokenKind::RightBracket, "']' after the array index");
                result.selectors.emplace_back(std::move(index));
            } else {
                return result;
            }
        }
    }

    // instance(NAME := value, NAME := value);
    Statement call() {
        const Token& instance = advance();
        advance(); // '('
        CallStatement s;
        s.instance = instance.text;
        if (!check(TokenKind::RightParen)) {
            do {
                const Token& name = expect(TokenKind::Identifier, "an input name like IN in the call");
                expect(TokenKind::Assign, fmt::format("':=' after '{}'", name.text));
                s.arguments.push_back(CallArgument{name.text, expression(), name.location});
            } while (match(TokenKind::Comma));
        }
        expect(TokenKind::RightParen, "')' to close the call");
        expect(TokenKind::Semicolon, "';' after the call");
        return Statement{std::move(s), instance.location};
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
        case TokenKind::Time:
            advance();
            return makeExpression(Literal{std::get<std::int64_t>(token.value), true}, token.location);
        case TokenKind::TypePrefix: return typedLiteral();
        case TokenKind::StringLiteral:
        case TokenKind::WStringLiteral: {
            advance();
            Literal literal;
            literal.type = token.kind == TokenKind::StringLiteral ? DataType::String : DataType::WString;
            literal.text = std::get<std::u16string>(token.value);
            return makeExpression(std::move(literal), token.location);
        }
        case TokenKind::Identifier: {
            const auto location = token.location;
            const auto name = token.text;
            if (peekNext().kind == TokenKind::LeftParen) {
                return makeExpression(callExpression(), location);
            }
            auto result = access();
            if (check(TokenKind::LeftParen)) {
                fail(peek(), fmt::format("function block calls are statements: write '{}(...);' on its own, "
                                         "then use its outputs like {}.Q",
                                         name, name));
            }
            return makeExpression(std::move(result), location);
        }
        case TokenKind::LeftParen: {
            advance();
            auto inner = expression();
            expect(TokenKind::RightParen, "')' to close the '('");
            return inner;
        }
        default: fail(token, fmt::format("expected an expression, found {}", describe(token)));
        }
    }

    // INT#5, INT#-5, WORD#16#FF00, REAL#2, BOOL#1: a literal with exactly that type.
    ExpressionPtr typedLiteral() {
        const Token prefix = advance();
        const auto type = *elementaryTypeFromName(prefix.text);
        const auto name = std::string(dataTypeName(type));
        if (isStringType(type) || isCharType(type)) {
            return typedText(prefix, type, name);
        }
        const bool negative = match(TokenKind::Minus);
        const Token value = advance();
        Literal literal;
        literal.type = type;
        const auto sign = [&](auto number) { return negative ? -number : number; };
        switch (value.kind) {
        case TokenKind::Integer: {
            const auto number = sign(std::get<std::int64_t>(value.value));
            if (type == DataType::Bool) {
                if (number != 0 && number != 1) {
                    fail(value, fmt::format("BOOL# takes 0, 1, TRUE or FALSE, not {}", number));
                }
                literal.value = number == 1;
            } else if (type == DataType::Real) {
                literal.value = static_cast<double>(number);
            } else {
                literal.value = number;
            }
            break;
        }
        case TokenKind::Real:
            if (type != DataType::Real) {
                fail(value, fmt::format("{}# needs an integer, not {}", name, value.text));
            }
            literal.value = sign(std::get<double>(value.value));
            break;
        case TokenKind::True:
        case TokenKind::False:
            if (type != DataType::Bool || negative) {
                fail(value, fmt::format("{}# needs a number, not {}", name, value.text));
            }
            literal.value = value.kind == TokenKind::True;
            break;
        default: fail(value, fmt::format("expected a value after {}#, found {}", name, describe(value)));
        }
        return makeExpression(std::move(literal), prefix.location);
    }

    // STRING#'abc', WSTRING#"abc", CHAR#'a', WCHAR#"a". A CHAR literal holds
    // the character's code, like an integer literal.
    ExpressionPtr typedText(const Token& prefix, DataType type, const std::string& name) {
        const bool wide = type == DataType::WString || type == DataType::WChar;
        const Token value = advance();
        if (value.kind != (wide ? TokenKind::WStringLiteral : TokenKind::StringLiteral)) {
            fail(value, fmt::format("{}# needs a {} literal, found {}", name, wide ? "\"double-quoted\"" : "'single-quoted'",
                                    describe(value)));
        }
        Literal literal;
        literal.type = type;
        const auto& text = std::get<std::u16string>(value.value);
        if (isCharType(type)) {
            if (text.size() != 1) {
                fail(value, fmt::format("{}# needs one character, not {}", name, text.size()));
            }
            literal.value = std::int64_t{text[0]};
        } else {
            literal.text = text;
        }
        return makeExpression(std::move(literal), prefix.location);
    }

    // NAME(arguments): either all by name (IN1 := a, IN2 := b) or all in order (a, b).
    CallExpression callExpression() {
        CallExpression call;
        call.name = advance().text;
        advance(); // '('
        if (!check(TokenKind::RightParen)) {
            const bool named = check(TokenKind::Identifier) && peekNext().kind == TokenKind::Assign;
            do {
                const auto location = peek().location;
                const bool thisNamed = check(TokenKind::Identifier) && peekNext().kind == TokenKind::Assign;
                if (thisNamed != named) {
                    fail(peek(), fmt::format("in a call to {}, give either every argument with its name (IN := ...) or "
                                             "none",
                                             call.name));
                }
                std::string argumentName;
                if (named) {
                    argumentName = advance().text;
                    advance(); // ':='
                }
                call.arguments.push_back(CallArgument{argumentName, expression(), location});
            } while (match(TokenKind::Comma));
        }
        expect(TokenKind::RightParen, fmt::format("')' to close the call to {}", call.name));
        return call;
    }

    // ------------------------------------------------------------ Token helpers

    const Token& peek() const { return tokens_[pos_]; }

    // The token `ahead` places after the next one (or EndOfFile).
    const Token& peekAt(std::size_t ahead) const { return tokens_[std::min(pos_ + ahead, tokens_.size() - 1)]; }
    const Token& peekNext() const { return peekAt(1); }

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
template <class Rule>
auto parseWith(std::string_view source, Rule rule) -> tl::expected<decltype(rule(std::declval<Parser&>())), Diagnostic> {
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

tl::expected<CompilationUnit, Diagnostic> parseProgram(std::string_view source) {
    return parseWith(source, [](Parser& parser) { return parser.compilationUnit(); });
}

tl::expected<std::vector<Pou>, Diagnostic> parseFunctionBlocks(std::string_view source) {
    return parseWith(source, [](Parser& parser) { return parser.functionBlocksOnly(); });
}

tl::expected<StatementList, Diagnostic> parseStatements(std::string_view source) {
    return parseWith(source, [](Parser& parser) { return parser.statementsOnly(); });
}

tl::expected<ExpressionPtr, Diagnostic> parseExpression(std::string_view source) {
    return parseWith(source, [](Parser& parser) { return parser.expressionOnly(); });
}

} // namespace softplc::st
