#pragma once
#include "st/interpreter.hpp"
#include <cstdlib>
#include <string_view>

// Which engine the tests run on. ctest runs the whole suite twice, once with
// SOFT_PLC_TEST_ENGINE=tree and once with SOFT_PLC_TEST_ENGINE=bytecode
// (the default when it's not set), so both engines pass every test.
inline softplc::st::Engine testEngine() {
    const char* value = std::getenv("SOFT_PLC_TEST_ENGINE");
    return value != nullptr && std::string_view(value) == "tree" ? softplc::st::Engine::TreeWalker
                                                                 : softplc::st::Engine::Bytecode;
}
