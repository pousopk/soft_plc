// st_parse: shows how the parser understands Structured Text.
//
//   st_parse examples/traffic_light.st      a whole file: FUNCTION_BLOCKs and a PROGRAM
//   st_parse examples/expressions.st        statements on their own
//   st_parse -e "a := b + 3 * c;"
//   st_parse --bytecode examples/benchmark.st   the compiled bytecode instead of the tree
//
// For a whole file it also runs the analyzer, so type errors are reported too.
#include "st/analyzer.hpp"
#include "st/ast.hpp"
#include "st/compiler.hpp"
#include "st/lexer.hpp"
#include "st/parser.hpp"
#include "util/file.hpp"
#include <fmt/format.h>
#include <string>
#include <string_view>

namespace {

int usage() {
    fmt::print(stderr, "usage: st_parse [--bytecode] <file.st>\n       st_parse -e \"<code>\"\n");
    return 2;
}

int reportError(const softplc::st::Diagnostic& diagnostic, std::string_view source) {
    fmt::print(stderr, "error: {}\n", softplc::st::formatDiagnostic(diagnostic, source));
    return 1;
}

bool isWholeFile(std::string_view source) {
    const auto tokens = softplc::st::tokenize(source);
    return tokens && (tokens->front().kind == softplc::st::TokenKind::Program ||
                      tokens->front().kind == softplc::st::TokenKind::FunctionBlock);
}

} // namespace

int main(int argc, char** argv) {
    std::string source;
    const bool bytecode = argc >= 2 && std::string_view(argv[1]) == "--bytecode";
    if (bytecode) {
        --argc;
        ++argv;
    }
    if (argc == 3 && std::string_view(argv[1]) == "-e") {
        source = argv[2];
    } else if (argc == 2) {
        auto contents = softplc::readFile(argv[1]);
        if (!contents) {
            fmt::print(stderr, "cannot open '{}'\n", argv[1]);
            return 1;
        }
        source = std::move(*contents);
    } else {
        return usage();
    }

    if (isWholeFile(source)) {
        const auto program = softplc::st::compileProgram(source);
        if (!program) {
            return reportError(program.error(), source);
        }
        if (bytecode) {
            for (const auto& functionBlock : program->functionBlocks) {
                fmt::print("FUNCTION_BLOCK {}\n{}\n", functionBlock.name,
                           softplc::st::disassemble(softplc::st::compile(functionBlock)));
            }
            fmt::print("PROGRAM {}\n{}", program->program.name,
                       softplc::st::disassemble(softplc::st::compile(program->program)));
            return 0;
        }
        fmt::print("{}", softplc::st::dumpTree(*program));
        return 0;
    }

    const auto statements = softplc::st::parseStatements(source);
    if (!statements) {
        return reportError(statements.error(), source);
    }
    for (const auto& statement : *statements) {
        fmt::print("{}\n", softplc::st::dumpTree(statement));
    }
    return 0;
}
