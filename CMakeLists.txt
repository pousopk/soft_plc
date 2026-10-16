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

# --- Snap7: the S7 protocol, for the S7 server (see cmake/snap7.cmake) ---
include(cmake/snap7.cmake)

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
  src/stdlib/timers.cpp
  src/stdlib/counters.cpp
  src/st/diagnostic.cpp
  src/st/lexer.cpp
  src/st/ast.cpp
  src/st/parser.cpp
  src/st/analyzer.cpp
  src/st/interpreter.cpp
  src/st/st_program.cpp
  src/st/builtins.cpp
  src/st/value_text.cpp
  src/monitor/monitor_service.cpp
  src/monitor/monitor_server.cpp
  src/monitor/monitor_client.cpp
  src/s7/s7_server.cpp
)
target_include_directories(soft_plc_core PUBLIC src)
target_link_libraries(soft_plc_core
  PUBLIC  soft_plc_options tl::expected Threads::Threads
  PRIVATE snap7_static
  PRIVATE fmt::fmt
)

# --- Demo executable ---
add_executable(soft_plc
  src/main.cpp
  src/demo/blink_program.cpp
  src/demo/console_io.cpp
)
target_link_libraries(soft_plc PRIVATE soft_plc_core fmt::fmt)

# --- Tools ---
add_executable(st_parse tools/st_parse.cpp)
target_link_libraries(st_parse PRIVATE soft_plc_core fmt::fmt)

# --- Monitoring GUI (optional: needs GLFW and OpenGL) ---
option(SOFT_PLC_BUILD_GUI "Build the soft_plc_monitor GUI (needs GLFW and OpenGL)" ON)
if(SOFT_PLC_BUILD_GUI)
  find_package(glfw3 QUIET)
  find_package(OpenGL QUIET)
endif()
if(SOFT_PLC_BUILD_GUI AND glfw3_FOUND AND OpenGL_FOUND)
  # Dear ImGui and ImPlot have no CMake support upstream: built as plain static libraries.
  FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.92.6
  )
  FetchContent_Declare(
    implot
    GIT_REPOSITORY https://github.com/epezent/implot.git
    GIT_TAG        v1.0
  )
  FetchContent_MakeAvailable(imgui implot)

  add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
    ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
  )
  target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends ${imgui_SOURCE_DIR}/misc/cpp
  )
  target_link_libraries(imgui PUBLIC glfw OpenGL::GL)

  add_library(implot STATIC ${implot_SOURCE_DIR}/implot.cpp ${implot_SOURCE_DIR}/implot_items.cpp)
  target_include_directories(implot PUBLIC ${implot_SOURCE_DIR})
  target_link_libraries(implot PUBLIC imgui)

  soft_plc_mark_system(imgui)
  soft_plc_mark_system(implot)

  add_executable(soft_plc_monitor
    tools/monitor_gui/main.cpp
    tools/monitor_gui/poller.cpp
    tools/monitor_gui/monitor_window.cpp
  )
  target_link_libraries(soft_plc_monitor PRIVATE soft_plc_core implot imgui fmt::fmt)
elseif(SOFT_PLC_BUILD_GUI)
  message(STATUS "soft_plc_monitor GUI not built: GLFW or OpenGL development files not found")
endif()

enable_testing()
add_subdirectory(tests)
