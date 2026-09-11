cmake_minimum_required(VERSION 3.16)
project(sigsim_oscillator CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# On MinGW toolchains, dynamically linking libstdc++/libgcc means the
# binary resolves those DLLs from whatever is first on PATH at runtime --
# on a machine with more than one MinGW distribution installed (e.g. Git
# for Windows' bundled mingw64 ahead of the compiling toolchain's own
# ucrt64/mingw64 on PATH), that can load an ABI-incompatible DLL and
# crash immediately (observed: segfault in std::ofstream's constructor).
# Statically linking these two removes the dependency entirely; applied to
# every executable target below via a helper function.
function(sigsim_harden_mingw target)
    if(MINGW)
        target_link_options(${target} PRIVATE -static-libgcc -static-libstdc++)
    endif()
endfunction()

add_executable(sigsim_dump src/main.cpp)
target_include_directories(sigsim_dump PRIVATE include)
target_compile_options(sigsim_dump PRIVATE -Wall -Wextra)
sigsim_harden_mingw(sigsim_dump)

# --- Phase 3: detectors ---------------------------------------------------

add_library(sigsim_detect INTERFACE)
target_include_directories(sigsim_detect INTERFACE include)

add_executable(detect_dump src/detect_dump.cpp)
target_link_libraries(detect_dump PRIVATE sigsim_detect)
target_compile_options(detect_dump PRIVATE -Wall -Wextra)
sigsim_harden_mingw(detect_dump)

add_executable(detect_sweep src/detect_sweep.cpp)
target_link_libraries(detect_sweep PRIVATE sigsim_detect)
target_compile_options(detect_sweep PRIVATE -Wall -Wextra)
sigsim_harden_mingw(detect_sweep)
