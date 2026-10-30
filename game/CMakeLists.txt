# The decomp's game code (Game, Movie, EA System and EA Sound libraries),
# compiled with clang for arm64, plus this repo's own game-side code
# (game/native/), as the game image FrontlineGame.so.

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

# The game is an image of its own (a Mach-O bundle the platform loads at
# start, frontline::LoadGame), so its static data can be put back as it was
# at startup and the game rebooted in place between levels (platform/src/os/
# reboot.cpp). It calls into the platform layer in the executable, found when
# it loads.
add_library(frontline_game MODULE ${FRONTLINE_GAME_SOURCES})
set_target_properties(frontline_game PROPERTIES
    OUTPUT_NAME FrontlineGame
    PREFIX ""
    SUFFIX ".so"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/game")

# Native-only game code of this repo's own (game/native/), built like the
# decomp's: the hooks it implements are declared in the shared headers.
target_sources(frontline_game PRIVATE
    # What the platform calls in the game image (native/entry.h)
    native/entry.cpp
    native/hint_icons.cpp
    native/prompts.cpp
    # Rewritten's enhanced enemy AI (native/ai.h)
    native/ai/aim.cpp
    native/ai/cover.cpp
    native/ai/director.cpp
    native/ai/drops.cpp
    native/ai/grenades.cpp
    native/ai/script_bias.cpp
    native/ai/state.cpp
    # Rewritten keeps the bodies of the dead (native/corpses.h)
    native/corpses.cpp)

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
# those inside the game image: exported, dyld would hand them to every
# framework in the process too. The platform layer and the app keep the
# system allocator.
target_link_options(frontline_game PRIVATE
    "LINKER:-unexported_symbol,__Znwm"
    "LINKER:-unexported_symbol,__Znam"
    "LINKER:-unexported_symbol,__ZnamPc"
    "LINKER:-unexported_symbol,__ZdlPv"
    "LINKER:-unexported_symbol,__ZdaPv"
    # The platform layer's functions are the executable's (frontline_game_host).
    "LINKER:-undefined,dynamic_lookup")

# The game's main() runs on a game thread (frontline::StartGame), so the
# executable's own main can be the app's.
set_source_files_properties("${_decomp}/src/game/Moh2.cpp" PROPERTIES COMPILE_DEFINITIONS "main=FrontlineGameMain")

# What an executable that runs the game links: the whole platform layer
# (the game image calls functions the executable itself never does) with its
# symbols exported for the game image to find, and the game image built
# alongside. FRONTLINE_GAME_IMAGE is where the build put it.
add_library(frontline_game_host INTERFACE)
target_link_libraries(frontline_game_host INTERFACE "$<LINK_LIBRARY:WHOLE_ARCHIVE,frontline_platform>")
target_link_options(frontline_game_host INTERFACE "LINKER:-export_dynamic")
target_compile_definitions(frontline_game_host INTERFACE "FRONTLINE_GAME_IMAGE=\"$<TARGET_FILE:frontline_game>\"")

