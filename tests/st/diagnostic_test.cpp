#include <doctest/doctest.h>
#include "st/diagnostic.hpp"

using namespace softplc::st;

TEST_CASE("formatDiagnostic quotes the line and puts a caret under the column") {
    const Diagnostic diagnostic{"expected an expression, found '*'", {2, 10}};
    const char* source = "x := 1;\na := b + * c;\ny := 2;";

    CHECK(formatDiagnostic(diagnostic, source) == "line 2, column 10: expected an expression, found '*'\n"
                                                  "  a := b + * c;\n"
                                                  "           ^");
}

TEST_CASE("formatDiagnostic keeps tabs so the caret lines up") {
    const Diagnostic diagnostic{"oops", {1, 3}};
    CHECK(formatDiagnostic(diagnostic, "\ta$") == "line 1, column 3: oops\n"
                                                  "  \ta$\n"
                                                  "  \t ^");
}
