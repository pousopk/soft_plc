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
  stdlib/ton_test.cpp
)
target_link_libraries(soft_plc_tests PRIVATE soft_plc_core doctest::doctest)
add_test(NAME soft_plc_tests COMMAND soft_plc_tests)
