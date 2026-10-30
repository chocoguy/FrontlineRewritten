# Checks the geometry of Rewritten's enemy AI (game/native/ai/ai_math.h)
# without the game:
#   cmake --build --preset ninja --target frontline_aitest
#   build/ninja/tests/aitest/frontline_aitest
add_executable(frontline_aitest EXCLUDE_FROM_ALL main.cpp)
target_include_directories(frontline_aitest PRIVATE "${PROJECT_SOURCE_DIR}/game/native/ai")
set_target_properties(frontline_aitest PROPERTIES CXX_STANDARD 14)
