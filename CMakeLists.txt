cmake_minimum_required(VERSION 3.21)
project(soft_plc CXX)

# Some fetched dependencies (e.g. doctest 2.4.x) declare a
# cmake_minimum_required below 3.5, which CMake 4.x refuses. This is CMake's
# documented workaround for configuring them anyway.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

option(SOFT_PLC_SANITIZERS "Build Debug builds with AddressSanitizer and UBSan" ON)

include(FetchContent)

# --- {fmt}: text formatting (GCC 11 has no <format>) ---
FetchContent_Declare(
  fmt
  GIT_REPOSITORY https://github.com/fmtlib/fmt.git
  GIT_TAG        11.1.4
)
FetchContent_MakeAvailable(fmt)

# --- tl::expected: value-or-error return type (std::expected is C++23) ---
FetchContent_Declare(
  tl-expected
  GIT_REPOSITORY https://github.com/TartanLlama/expected.git
  GIT_TAG        v1.1.0
)
set(EXPECTED_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(EXPECTED_BUILD_PACKAGE OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(tl-expected)

find_package(Threads REQUIRED)

# Treat third-party headers as system headers so our strict warnings below
# apply only to our own code. (CMake >= 3.25 has FetchContent's SYSTEM option
# for this, but Ubuntu 22.04 ships 3.22.)
function(soft_plc_mark_system target)
  get_target_property(dirs ${target} INTERFACE_INCLUDE_DIRECTORIES)
  set_target_properties(${target} PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${dirs}")
endfunction()
soft_plc_mark_system(fmt)
soft_plc_mark_system(expected)

# --- Shared compile settings for our own targets ---
add_library(soft_plc_options INTERFACE)
target_compile_options(soft_plc_options INTERFACE
  -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
)
if(SOFT_PLC_SANITIZERS)
  target_compile_options(soft_plc_options INTERFACE
    $<$<CONFIG:Debug>:-fsanitize=address,undefined -fno-omit-frame-pointer>
  )
  target_link_options(soft_plc_options INTERFACE
    $<$<CONFIG:Debug>:-fsanitize=address,undefined>
  )
endif()

# --- Core library: everything except the demo executable ---
add_library(soft_plc_core STATIC
  src/core/address.cpp
  src/core/process_image.cpp
  src/core/scan_cycle.cpp
  src/stdlib/ton.cpp
)
target_include_directories(soft_plc_core PUBLIC src)
target_link_libraries(soft_plc_core
  PUBLIC  soft_plc_options tl::expected Threads::Threads
  PRIVATE fmt::fmt
)

# --- Demo executable ---
add_executable(soft_plc
  src/main.cpp
  src/demo/blink_program.cpp
  src/demo/console_io.cpp
)
target_link_libraries(soft_plc PRIVATE soft_plc_core fmt::fmt)

enable_testing()
add_subdirectory(tests)
