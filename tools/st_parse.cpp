// st_parse: shows how the parser understands Structured Text.
//
//   st_parse examples/expressions.st
//   st_parse -e "a := b + 3 * c;"
#include "st/ast.hpp"
#include "st/parser.hpp"
#include <fmt/format.h>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

namespace {

int usage() {
    fmt::print(stderr, "usage: st_parse <file.st>\n       st_parse -e \"<code>\"\n");
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    std::string source;
    if (argc == 3 && std::string_view(argv[1]) == "-e") {
        source = argv[2];
    } else if (argc == 2) {
        std::ifstream file(argv[1]);
        if (!file) {
            fmt::print(stderr, "cannot open '{}'\n", argv[1]);
            return 1;
        }
        std::ostringstream contents;
        contents << file.rdbuf();
        source = contents.str();
    } else {
        return usage();
    }

    const auto statements = softplc::st::parseStatements(source);
    if (!statements) {
        fmt::print(stderr, "error: {}\n", softplc::st::formatDiagnostic(statements.error(), source));
        return 1;
    }

    for (const auto& statement : *statements) {
        fmt::print("{} := {};\n", statement.target, softplc::st::formatExpression(*statement.value));
        fmt::print("{}\n", softplc::st::dumpTree(statement));
    }
    return 0;
}
