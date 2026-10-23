# The platform layer: native replacements for the GameCube system libraries
# (OS, DVD, VI, PAD, CARD, AX/AR/DSP) and the 32-bit address map.

add_library(frontline_platform STATIC
    src/address.cpp
    src/audio.cpp
    src/card.cpp
    src/dvd.cpp
    src/game_hooks.cpp
    src/gx/gx.cpp
    src/gx/gx_metal.mm
    src/gx/gx_shader.cpp
    src/gx/gx_texture.cpp
    src/gx/gx_vertex.cpp
    src/mtx.cpp
    src/os/crash.cpp
    src/os/memory.cpp
    src/os/misc.cpp
    src/os/thread.cpp
    src/os/time.cpp
    src/pad.cpp
    src/vi.cpp)

target_include_directories(frontline_platform PUBLIC include)

# native/address.h and the other shared game headers. -idirafter, as for the
# game itself, so the decomp's own C library headers never shadow Apple's.
target_compile_options(frontline_platform PUBLIC "SHELL:-idirafter ${FRONTLINE_DECOMP_DIR}/include")

target_compile_features(frontline_platform PUBLIC cxx_std_20)
target_compile_definitions(frontline_platform PUBLIC FRONTLINE_NATIVE=1)
target_include_directories(frontline_platform PRIVATE src)

# The GX backend is Objective-C++ on Metal.
enable_language(OBJCXX)
set_source_files_properties(src/gx/gx_metal.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
target_link_libraries(frontline_platform PUBLIC "-framework Metal" "-framework Foundation")
