FetchContent_Declare(
  doctest
  GIT_REPOSITORY https://github.com/doctest/doctest.git
  GIT_TAG        v2.4.11
)
FetchContent_MakeAvailable(doctest)
soft_plc_mark_system(doctest)

add_executable(soft_plc_tests
  test_main.cpp
  core/address_test.cpp
  core/process_image_test.cpp
  core/scan_cycle_test.cpp
  stdlib/timers_test.cpp
  stdlib/counters_test.cpp
  stdlib/edges_test.cpp
  st/function_block_test.cpp
  st/lexer_test.cpp
  st/parser_test.cpp
  st/diagnostic_test.cpp
  st/statement_parser_test.cpp
  st/analyzer_test.cpp
  st/interpreter_test.cpp
  st/st_program_test.cpp
  monitor/monitor_test.cpp
  s7/s7_server_test.cpp
  st/engine_equivalence_test.cpp
)
target_link_libraries(soft_plc_tests PRIVATE soft_plc_core snap7_static doctest::doctest fmt::fmt)
target_include_directories(soft_plc_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# Lets tests find the example programs.
target_compile_definitions(soft_plc_tests PRIVATE SOFT_PLC_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

# The whole suite runs once per engine (see tests/support/engine.hpp).
add_test(NAME soft_plc_tests_bytecode COMMAND soft_plc_tests)
set_tests_properties(soft_plc_tests_bytecode PROPERTIES ENVIRONMENT "SOFT_PLC_TEST_ENGINE=bytecode")
add_test(NAME soft_plc_tests_tree COMMAND soft_plc_tests)
set_tests_properties(soft_plc_tests_tree PROPERTIES ENVIRONMENT "SOFT_PLC_TEST_ENGINE=tree")
