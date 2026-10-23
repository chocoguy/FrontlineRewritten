# Checks the native loaders of GameCube-only data against the user's disc:
#   cmake --build --preset ninja --target frontline_datacheck
#   build/ninja/tests/datacheck/frontline_datacheck FrontlineDecompiled/orig/GMFE69/disc
add_executable(frontline_datacheck EXCLUDE_FROM_ALL main.cpp aems.cpp font.cpp mpf.cpp scr.cpp viv.cpp
    # The font compiled into the game (FONTstandfile).
    ${FRONTLINE_DECOMP_DIR}/src/ea/sys/fontstdf.cpp)
target_link_libraries(frontline_datacheck PRIVATE frontline_platform)
# Same environment as the game code, since it includes game headers.
target_compile_options(frontline_datacheck PRIVATE
    -fno-strict-aliasing -fsigned-char
    "SHELL:-include ${PROJECT_SOURCE_DIR}/platform/include/msl_compat.h")
