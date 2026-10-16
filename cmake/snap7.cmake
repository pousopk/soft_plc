# Snap7: an open-source implementation of the Siemens S7 protocol (client and
# server). It has no usable CMake build of its own, so it's built here as a
# static library. The same recipe and patches as in plc_gui.
FetchContent_Declare(
  snap7
  GIT_REPOSITORY https://github.com/SCADACS/snap7.git
  GIT_TAG        f6ff90317ca5d54250f4dcd29209689a74e26d82
  PATCH_COMMAND  ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_LIST_DIR}/patch_snap7.cmake
)
FetchContent_MakeAvailable(snap7)

add_library(snap7_static STATIC
  ${snap7_SOURCE_DIR}/src/sys/snap_msgsock.cpp
  ${snap7_SOURCE_DIR}/src/sys/snap_sysutils.cpp
  ${snap7_SOURCE_DIR}/src/sys/snap_tcpsrvr.cpp
  ${snap7_SOURCE_DIR}/src/sys/snap_threads.cpp
  ${snap7_SOURCE_DIR}/src/core/s7_client.cpp
  ${snap7_SOURCE_DIR}/src/core/s7_isotcp.cpp
  ${snap7_SOURCE_DIR}/src/core/s7_partner.cpp
  ${snap7_SOURCE_DIR}/src/core/s7_peer.cpp
  ${snap7_SOURCE_DIR}/src/core/s7_server.cpp
  ${snap7_SOURCE_DIR}/src/core/s7_text.cpp
  ${snap7_SOURCE_DIR}/src/core/s7_micro_client.cpp
  ${snap7_SOURCE_DIR}/src/lib/snap7_libmain.cpp
)
target_include_directories(snap7_static SYSTEM PUBLIC
  ${snap7_SOURCE_DIR}/src/sys
  ${snap7_SOURCE_DIR}/src/core
  ${snap7_SOURCE_DIR}/src/lib
  ${snap7_SOURCE_DIR}/examples/plain-c # the public snap7.h (Cli_* and Srv_* functions)
)
target_link_libraries(snap7_static PUBLIC Threads::Threads)
# Third-party code: don't drown our build output in its warnings.
target_compile_options(snap7_static PRIVATE -w)
