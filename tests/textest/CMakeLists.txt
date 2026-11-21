# Checks Rewritten's made mipmaps (platform/src/gx/gx_texture.h Downsample,
# AlphaTestCoverage, KeepCoverage) without the game:
#   cmake --build --preset ninja --target frontline_textest
#   build/ninja/tests/textest/frontline_textest
add_executable(frontline_textest EXCLUDE_FROM_ALL main.cpp "${PROJECT_SOURCE_DIR}/platform/src/gx/gx_texture.cpp")
target_include_directories(frontline_textest PRIVATE "${PROJECT_SOURCE_DIR}/platform/src/gx")
set_target_properties(frontline_textest PROPERTIES CXX_STANDARD 17)
