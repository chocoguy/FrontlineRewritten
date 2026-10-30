# The game without a window or sound, for smoke tests:
#   cmake --build --preset ninja
#   build/ninja/apps/headless/frontline_headless FrontlineDecompiled/orig/GMFE69/disc 10
add_executable(frontline_headless main.cpp)
target_link_libraries(frontline_headless PRIVATE frontline_game_host)
add_dependencies(frontline_headless frontline_game)
