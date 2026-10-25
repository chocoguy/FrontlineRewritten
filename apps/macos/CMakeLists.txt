# The macOS app: a SwiftUI window with the game's Metal view, keyboard and
# game controller input. Swift reaches the game through Bridge/ (its bridging
# header), a small Objective-C++ library over the game and the platform layer.

add_library(frontline_bridge STATIC Bridge/FrontlineBridge.mm)
set_source_files_properties(Bridge/FrontlineBridge.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
target_link_libraries(frontline_bridge PUBLIC frontline_game frontline_platform)

set(FRONTLINE_BUNDLE_ID "io.github.chocoguy.FrontlineRewritten"
    CACHE STRING "Bundle identifier for the macOS app")

add_executable(FrontlineMac MACOSX_BUNDLE
    Sources/FrontlineApp.swift
    Sources/GameView.swift
    Sources/Input.swift
    Sources/Renderer.swift)

set_target_properties(FrontlineMac PROPERTIES
    OUTPUT_NAME "Frontline Rewritten"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
    MACOSX_BUNDLE_GUI_IDENTIFIER "${FRONTLINE_BUNDLE_ID}"
    MACOSX_BUNDLE_BUNDLE_NAME "Frontline Rewritten"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${FRONTLINE_BUNDLE_ID}"
    # Sign to run locally; nothing here is distributed as a binary (see ROADMAP gotcha 12).
    XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "-"
    XCODE_ATTRIBUTE_CODE_SIGN_STYLE Manual
    XCODE_ATTRIBUTE_ENABLE_HARDENED_RUNTIME YES
    XCODE_GENERATE_SCHEME ON)

# Swift 6 language mode. The Xcode generator would turn Swift_LANGUAGE_VERSION
# into a -swift-version flag next to SWIFT_VERSION = 4.0, and Xcode warns that
# they disagree, so Xcode gets its own build setting instead.
set(_bridging_header "${CMAKE_CURRENT_SOURCE_DIR}/Bridge/FrontlineBridge.h")
if(CMAKE_GENERATOR STREQUAL "Xcode")
    set_target_properties(FrontlineMac PROPERTIES
        XCODE_ATTRIBUTE_SWIFT_VERSION 6.0
        XCODE_ATTRIBUTE_SWIFT_OBJC_BRIDGING_HEADER "${_bridging_header}")
else()
    set_target_properties(FrontlineMac PROPERTIES Swift_LANGUAGE_VERSION 6)
    target_compile_options(FrontlineMac PRIVATE "SHELL:-import-objc-header ${_bridging_header}")
endif()

target_link_libraries(FrontlineMac PRIVATE
    frontline_bridge
    "-framework AppKit"
    "-framework CoreHaptics"
    "-framework GameController"
    "-framework SwiftUI"
    "-framework Metal"
    "-framework MetalKit")
