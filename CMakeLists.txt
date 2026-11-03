cmake_minimum_required(VERSION 3.31)

# Apple silicon only, on the newest SDK. These have to be set before project().
# macOS by default; the tvos and tvos-sim presets set CMAKE_SYSTEM_NAME=tvOS
# (docs/port/phase3-status.md).
set(CMAKE_OSX_ARCHITECTURES arm64 CACHE STRING "Target architectures")
set(CMAKE_OSX_DEPLOYMENT_TARGET 27.0 CACHE STRING "Minimum macOS or tvOS version")

project(FrontlineRewritten
    VERSION 0.0.1
    DESCRIPTION "Native Apple port of Medal of Honor: Frontline (GMFE69)"
    LANGUAGES C CXX OBJCXX)

if(NOT APPLE)
    message(FATAL_ERROR "FrontlineRewritten only builds for Apple platforms.")
endif()

option(FRONTLINE_BUILD_APP "Build the app shell" ON)

if(CMAKE_SYSTEM_NAME STREQUAL "tvOS")
    set(FRONTLINE_TVOS ON)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(FRONTLINE_TVOS OFF)
else()
    message(FATAL_ERROR "Unsupported CMAKE_SYSTEM_NAME ${CMAKE_SYSTEM_NAME}: macOS or tvOS only.")
endif()

# The team that signs device builds; empty for the simulator and the Mac's
# ad hoc signing.
set(FRONTLINE_DEVELOPMENT_TEAM "" CACHE STRING "Apple developer Team ID for device builds")
if(FRONTLINE_TVOS)
    set(CMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${FRONTLINE_DEVELOPMENT_TEAM}")
endif()

set(FRONTLINE_DECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/FrontlineDecompiled"
    CACHE PATH "The decomp checkout that supplies the game code")

if(NOT EXISTS "${FRONTLINE_DECOMP_DIR}/configure.py")
    message(FATAL_ERROR
        "No decomp at ${FRONTLINE_DECOMP_DIR}. Run: git submodule update --init")
endif()

add_subdirectory(platform)
add_subdirectory(game)

# The tests and the headless runner are Mac tools.
if(NOT FRONTLINE_TVOS)
    add_subdirectory(tests/datacheck)
    add_subdirectory(tests/ostest)
    add_subdirectory(tests/aitest)
    add_subdirectory(tests/hfrtest)
    add_subdirectory(tests/limitertest)
    add_subdirectory(apps/headless)
endif()

if(FRONTLINE_BUILD_APP)
    enable_language(Swift)
    add_subdirectory(apps/shared)
    if(FRONTLINE_TVOS)
        add_subdirectory(apps/ios)
    else()
        add_subdirectory(apps/macos)
    endif()
endif()
