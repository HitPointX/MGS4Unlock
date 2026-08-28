cmake_minimum_required(VERSION 3.20)
project(MGS4Unlock CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Which system DLL we impersonate. mgs4.exe imports all of these directly, so no
# third-party ASI loader is needed. dbghelp is the default: its only consumer in
# the process is crash handling, so a proxy in front of it has a small blast
# radius (winmm, by contrast, sits in front of Wine's audio path).
set(PROXY_TARGET "dbghelp" CACHE STRING "System DLL to impersonate: dbghelp | winmm")
set_property(CACHE PROXY_TARGET PROPERTY STRINGS dbghelp winmm)

add_library(MGS4Unlock SHARED
    src/dllmain.cpp
    src/proxy/proxy.cpp
    src/proxy/${PROXY_TARGET}.def
    src/core/log.cpp
    src/core/config.cpp
    src/core/memory.cpp
    src/core/module.cpp
    src/core/pdata.cpp
    src/dumper/dumper.cpp
    src/mgs4/picker.cpp
)

target_include_directories(MGS4Unlock PRIVATE src)
target_compile_definitions(MGS4Unlock PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    PROXY_TARGET_NAME="${PROXY_TARGET}"
    PROXY_${PROXY_TARGET}
)
target_compile_options(MGS4Unlock PRIVATE -m64 -O2 -Wall -Wextra -Wno-cast-function-type)

# -static so the DLL carries no libstdc++/libgcc/winpthread dependency.
# --exclude-all-symbols keeps mingw from auto-exporting our internals, so the
# .def file above is the complete and exact export surface.
target_link_options(MGS4Unlock PRIVATE
    -m64 -static
    -Wl,--enable-stdcall-fixup
    -Wl,--exclude-all-symbols
)
target_link_libraries(MGS4Unlock PRIVATE shlwapi)

# The output must be named exactly like the DLL we impersonate.
set_target_properties(MGS4Unlock PROPERTIES
    PREFIX ""
    OUTPUT_NAME "${PROXY_TARGET}"
    SUFFIX ".dll"
)
