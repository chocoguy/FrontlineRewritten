# Checks Rewritten's soft limiter on the speakers' mix
# (platform/src/audio_limiter.h) without the game:
#   cmake --build --preset ninja --target frontline_limitertest
#   build/ninja/tests/limitertest/frontline_limitertest
add_executable(frontline_limitertest EXCLUDE_FROM_ALL main.cpp "${PROJECT_SOURCE_DIR}/platform/src/audio_limiter.cpp")
target_include_directories(frontline_limitertest PRIVATE "${PROJECT_SOURCE_DIR}/platform/src")
set_target_properties(frontline_limitertest PROPERTIES CXX_STANDARD 17)
