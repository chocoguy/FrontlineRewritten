# The decomp's game code (Game, Movie, EA System and EA Sound libraries),
# compiled with clang for arm64.
#
# It doesn't build yet: the build errors are the Phase 1 work list (see
# tools/build_report.py). Until it builds, it's left out of the default build;
# build it explicitly with `--target frontline_game`.

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(_decomp "${FRONTLINE_DECOMP_DIR}")
set(_sources_cmake "${CMAKE_CURRENT_BINARY_DIR}/GameSources.cmake")

execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/gen_sources.py"
            "${_decomp}" "${_sources_cmake}"
    COMMAND_ERROR_IS_FATAL ANY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_decomp}/configure.py" "${PROJECT_SOURCE_DIR}/tools/gen_sources.py")
include("${_sources_cmake}")

list(TRANSFORM FRONTLINE_GAME_SOURCES PREPEND "${_decomp}/")
list(TRANSFORM FRONTLINE_GAME_CXX_C_SOURCES PREPEND "${_decomp}/")
set_source_files_properties(${FRONTLINE_GAME_CXX_C_SOURCES} PROPERTIES LANGUAGE CXX)

# A few units include data extracted from the user's DOL by the GameCube build
# (the memory card icon and the built-in font).
set(_extracted "${_decomp}/build/GMFE69/include")
if(NOT EXISTS "${_extracted}/game/Moh2_InternalFont.inc")
    message(WARNING
        "${_extracted} is missing. Build the GameCube reference first "
        "(see FrontlineDecompiled/README.md); Moh2.cpp and MemoryCardGC.cpp need it.")
endif()

add_library(frontline_game STATIC EXCLUDE_FROM_ALL ${FRONTLINE_GAME_SOURCES})

set_target_properties(frontline_game PROPERTIES
    C_STANDARD 11
    C_EXTENSIONS ON
    CXX_STANDARD 14
    CXX_EXTENSIONS ON
    # Keep the Xcode project browsable by the decomp's own folder layout.
    XCODE_GENERATE_SCHEME ON)

target_compile_definitions(frontline_game PRIVATE
    FRONTLINE_NATIVE=1
    BUILD_VERSION=0
    VERSION_GMFE69)

# The game's MSL headers (string.h, math.h, ...) live at the root of the
# decomp's include/ folder. -idirafter puts that folder after the SDK, so the
# real C library wins and the game's own headers (game/, ea/, dolphin/, ...)
# are still found.
target_compile_options(frontline_game PRIVATE
    "SHELL:-include ${PROJECT_SOURCE_DIR}/platform/include/msl_compat.h"
    "SHELL:-idirafter ${_decomp}/include"
    "SHELL:-idirafter ${_extracted}"
    # See docs/architecture/0004-compiler-rules.md.
    -fno-strict-aliasing
    -fwrapv
    -ffp-contract=off
    -fsigned-char
    # The game replaces operator new/delete (game/memory.cpp) but not the sized
    # delete, so every delete must go through the unsized one.
    -fno-sized-deallocation
    -ferror-limit=0
    # Old code: silence warnings, except ones that are always 64-bit bugs.
    # (-w would silence those too, even when promoted to errors.)
    -Wno-everything
    -Werror=int-to-pointer-cast
    -Werror=pointer-to-int-cast
    -Werror=int-conversion
    -Werror=incompatible-pointer-types
    -Werror=implicit-function-declaration
    -Werror=return-type)

source_group(TREE "${_decomp}/src" PREFIX "src" FILES ${FRONTLINE_GAME_SOURCES})

# The game replaces the global operator new/delete (game/memory.cpp). Keep
# those inside the executable: exported, dyld would hand them to every
# framework in the process too.
target_link_options(frontline_game INTERFACE
    "LINKER:-unexported_symbol,__Znwm"
    "LINKER:-unexported_symbol,__Znam"
    "LINKER:-unexported_symbol,__ZnamPc"
    "LINKER:-unexported_symbol,__ZdlPv"
    "LINKER:-unexported_symbol,__ZdaPv")

# The game's main() runs on a game thread (frontline::StartGame), so the
# executable's own main can be the app's.
set_source_files_properties("${_decomp}/src/game/Moh2.cpp" PROPERTIES COMPILE_DEFINITIONS "main=FrontlineGameMain")
target_link_libraries(frontline_game INTERFACE frontline_platform)
