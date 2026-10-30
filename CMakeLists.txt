cmake_minimum_required(VERSION 3.31)

# Apple silicon only, on the newest SDK. These have to be set before project().
set(CMAKE_OSX_ARCHITECTURES arm64 CACHE STRING "Target architectures")
set(CMAKE_OSX_DEPLOYMENT_TARGET 27.0 CACHE STRING "Minimum macOS version")

project(FrontlineRewritten
    VERSION 0.0.1
    DESCRIPTION "Native Apple port of Medal of Honor: Frontline (GMFE69)"
    LANGUAGES C CXX OBJCXX)

if(NOT APPLE)
    message(FATAL_ERROR "FrontlineRewritten only builds for Apple platforms.")
endif()

option(FRONTLINE_BUILD_APP "Build the macOS app shell" ON)

set(FRONTLINE_DECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/FrontlineDecompiled"
    CACHE PATH "The decomp checkout that supplies the game code")

if(NOT EXISTS "${FRONTLINE_DECOMP_DIR}/configure.py")
    message(FATAL_ERROR
        "No decomp at ${FRONTLINE_DECOMP_DIR}. Run: git submodule update --init")
endif()

add_subdirectory(platform)
add_subdirectory(game)
add_subdirectory(tests/datacheck)
add_subdirectory(tests/ostest)
add_subdirectory(tests/aitest)
add_subdirectory(tests/hfrtest)
add_subdirectory(tests/limitertest)
add_subdirectory(apps/headless)

if(FRONTLINE_BUILD_APP)
    enable_language(Swift)
    add_subdirectory(apps/macos)
endif()
