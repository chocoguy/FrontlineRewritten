# Checks the platform layer's OS library (threads, alarms, interrupts, heaps)
# against the console's rules, without the game:
#   cmake --build --preset ninja --target frontline_ostest
#   build/ninja/tests/ostest/frontline_ostest
add_executable(frontline_ostest EXCLUDE_FROM_ALL main.cpp)
target_link_libraries(frontline_ostest PRIVATE frontline_platform)
