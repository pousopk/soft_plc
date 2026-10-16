# Patches for the Snap7 sources, run by FetchContent in the Snap7 source
# directory (the current directory in script mode). Each patch checks for its
# own marker first, so running it again changes nothing.

function(patch_once target_file marker old_text new_text)
    file(READ "${target_file}" file_contents)
    string(FIND "${file_contents}" "${marker}" already_patched)
    if(already_patched EQUAL -1)
        string(REPLACE "${old_text}" "${new_text}" file_contents "${file_contents}")
        file(WRITE "${target_file}" "${file_contents}")
    endif()
endfunction()

set(snap7_src "${CMAKE_CURRENT_SOURCE_DIR}/src")

# s7_partner.h has a file-scope `using namespace std;`, which since C++17
# makes the unqualified `byte` used all over Snap7 ambiguous with std::byte.
patch_once(
    "${snap7_src}/core/s7_partner.h"
    "removed: collides with std::byte"
    "using namespace std;"
    "// using namespace std; -- removed: collides with std::byte under C++17+"
)

# PS7AreaContainer's constructor allocates `new PS7Area[size]` (an array of
# pointers) but zeroes only `size` bytes instead of `size * sizeof(PS7Area)`.
# The unzeroed slots are later read as live pointers and freed on server
# teardown: a crash. Found with AddressSanitizer (in plc_gui).
patch_once(
    "${snap7_src}/core/s7_server.cpp"
    "memset(area, 0, size * sizeof(PS7Area))"
    "memset(area, 0, size);"
    "memset(area, 0, size * sizeof(PS7Area)); // patched: was `size` (bytes), undersizing a pointer array"
)

# PS7AreaContainer::Dispose() frees that array with `delete` instead of
# `delete[]`: undefined behaviour, also found with AddressSanitizer.
patch_once(
    "${snap7_src}/core/s7_server.cpp"
    "patched: was `delete area`"
    "    delete area;
}"
    "    delete[] area; // patched: was `delete area` on a `new PS7Area[size]` array (alloc-dealloc mismatch)
}"
)

# TS7Worker::SZLSubFuncRead() is declared `bool` but falls off its end on the
# normal path (subfunction 0x01, "read"). That's undefined behaviour; GCC 15
# compiles it to a trap instruction, so the server crashed with SIGILL as soon
# as a client asked for the CPU state (Cli_GetPlcStatus). Found by soft_plc's
# tests. Its caller treats `true` as "go on and read the SZL".
patch_once(
    "${snap7_src}/core/s7_server.cpp"
    "patched: missing return"
    "      SZLNotAvailable();
      return false;
  };
}"
    "      SZLNotAvailable();
      return false;
  };
  return true; // patched: missing return (undefined behaviour, a SIGILL with GCC 15)
}"
)
