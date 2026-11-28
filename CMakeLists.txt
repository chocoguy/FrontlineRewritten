cmake_minimum_required(VERSION 3.31)

# Apple silicon only, on the newest SDK. These have to be set before project().
# macOS by default; the tvos and tvos-sim presets set CMAKE_SYSTEM_NAME=tvOS
# (docs/port/phase3-status.md), and the ios and ios-sim presets iOS
# (docs/port/ios-status.md).
set(CMAKE_OSX_ARCHITECTURES arm64 CACHE STRING "Target architectures")
set(CMAKE_OSX_DEPLOYMENT_TARGET 27.0 CACHE STRING "Minimum macOS, iOS or tvOS version")

# The release being built: tools/tvos/port passes the tag (0.3.0, or 0.3.0-rc1)
# and its commit, which go in the apps' Info.plists, so the installer can tell
# what's installed. The bundle version is the numeric part as one build number.
set(FRONTLINE_VERSION "0.0.1" CACHE STRING "The release version (the tag without its v)")
set(FRONTLINE_COMMIT "" CACHE STRING "The commit built (short hash)")
if(NOT FRONTLINE_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)")
    message(FATAL_ERROR "FRONTLINE_VERSION ${FRONTLINE_VERSION} isn't a version like 0.3.0")
endif()
set(_numeric "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${CMAKE_MATCH_3}")
math(EXPR FRONTLINE_BUILD_NUMBER "${CMAKE_MATCH_1} * 10000 + ${CMAKE_MATCH_2} * 100 + ${CMAKE_MATCH_3}")

project(FrontlineRewritten
    VERSION ${_numeric}
    DESCRIPTION "Native Apple port of Medal of Honor: Frontline (GMFE69)"
    LANGUAGES C CXX OBJCXX)

if(NOT APPLE)
    message(FATAL_ERROR "FrontlineRewritten only builds for Apple platforms.")
endif()

option(FRONTLINE_BUILD_APP "Build the app shell" ON)

# FRONTLINE_UIKIT: the UIKit app (apps/ios), on Apple TV (FRONTLINE_TVOS) or
# iPhone and iPad (FRONTLINE_IOS); otherwise the Mac app.
set(FRONTLINE_TVOS OFF)
set(FRONTLINE_IOS OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "tvOS")
    set(FRONTLINE_TVOS ON)
elseif(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(FRONTLINE_IOS ON)
elseif(NOT CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    message(FATAL_ERROR "Unsupported CMAKE_SYSTEM_NAME ${CMAKE_SYSTEM_NAME}: macOS, iOS or tvOS only.")
endif()
if(FRONTLINE_TVOS OR FRONTLINE_IOS)
    set(FRONTLINE_UIKIT ON)
else()
    set(FRONTLINE_UIKIT OFF)
endif()

# The team that signs device builds; empty for the simulator and the Mac's
# ad hoc signing.
set(FRONTLINE_DEVELOPMENT_TEAM "" CACHE STRING "Apple developer Team ID for device builds")
if(FRONTLINE_UIKIT)
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
if(NOT FRONTLINE_UIKIT)
    add_subdirectory(tests/datacheck)
    add_subdirectory(tests/ostest)
    add_subdirectory(tests/aitest)
    add_subdirectory(tests/hfrtest)
    add_subdirectory(tests/limitertest)
    add_subdirectory(tests/textest)
    add_subdirectory(apps/headless)
endif()

if(FRONTLINE_BUILD_APP)
    enable_language(Swift)
    add_subdirectory(apps/shared)
    if(FRONTLINE_UIKIT)
        add_subdirectory(apps/ios)
    else()
        add_subdirectory(apps/macos)
        # Frontline Installer builds with the Xcode generator only (its asset catalog); its logic
        # (no UI) is tested with any generator.
        set(FRONTLINE_INSTALLER_LOGIC_SOURCES
            "${CMAKE_CURRENT_SOURCE_DIR}/apps/installer/Sources/PortEvent.swift"
            "${CMAKE_CURRENT_SOURCE_DIR}/apps/installer/Sources/Releases.swift"
            "${CMAKE_CURRENT_SOURCE_DIR}/apps/installer/Sources/SemVer.swift"
            "${CMAKE_CURRENT_SOURCE_DIR}/apps/installer/Sources/Workspace.swift")
        add_subdirectory(tests/installertest)
        if(CMAKE_GENERATOR STREQUAL "Xcode")
            add_subdirectory(apps/installer)
        endif()
    endif()
endif()
