# Checks how the high frame rate moves frames drawn between the game's
# (platform/src/gx/frame_motion.h) without the game:
#   cmake --build --preset ninja --target frontline_hfrtest
#   build/ninja/tests/hfrtest/frontline_hfrtest
add_executable(frontline_hfrtest EXCLUDE_FROM_ALL main.cpp "${PROJECT_SOURCE_DIR}/platform/src/gx/frame_motion.cpp")
target_include_directories(frontline_hfrtest PRIVATE "${PROJECT_SOURCE_DIR}/platform/src/gx")
set_target_properties(frontline_hfrtest PROPERTIES CXX_STANDARD 17)
