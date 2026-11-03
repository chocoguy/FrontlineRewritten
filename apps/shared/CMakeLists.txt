# What the macOS and tvOS apps share: Bridge/ (a small Objective-C++ library
# over the game and the platform layer, and the Swift apps' bridging header),
# and Swift sources each app compiles in with its own.

add_library(frontline_bridge STATIC Bridge/FrontlineBridge.mm)
set_source_files_properties(Bridge/FrontlineBridge.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
target_link_libraries(frontline_bridge PUBLIC frontline_game_host)

set(FRONTLINE_BRIDGING_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/Bridge/FrontlineBridge.h" PARENT_SCOPE)
set(FRONTLINE_SHARED_SWIFT
    "${CMAKE_CURRENT_SOURCE_DIR}/Controllers.swift"
    "${CMAKE_CURRENT_SOURCE_DIR}/PadBindings.swift"
    "${CMAKE_CURRENT_SOURCE_DIR}/PadBindingsView.swift"
    "${CMAKE_CURRENT_SOURCE_DIR}/Presenter.swift"
    "${CMAKE_CURRENT_SOURCE_DIR}/SaveSync.swift"
    "${CMAKE_CURRENT_SOURCE_DIR}/SettingsKeys.swift"
    PARENT_SCOPE)

# Sets up `target`, a Swift app, to reach the game through the bridge: Swift 6,
# the bridging header, and the game image built alongside.
function(frontline_swift_app target)
    target_sources(${target} PRIVATE ${FRONTLINE_SHARED_SWIFT})
    source_group("Shared" FILES ${FRONTLINE_SHARED_SWIFT})
    # Swift 6 language mode. The Xcode generator would turn
    # Swift_LANGUAGE_VERSION into a -swift-version flag next to SWIFT_VERSION =
    # 4.0, and Xcode warns that they disagree, so Xcode gets its own build
    # setting instead.
    if(CMAKE_GENERATOR STREQUAL "Xcode")
        set_target_properties(${target} PROPERTIES
            XCODE_ATTRIBUTE_SWIFT_VERSION 6.0
            XCODE_ATTRIBUTE_SWIFT_OBJC_BRIDGING_HEADER "${FRONTLINE_BRIDGING_HEADER}")
    else()
        set_target_properties(${target} PROPERTIES Swift_LANGUAGE_VERSION 6)
        target_compile_options(${target} PRIVATE "SHELL:-import-objc-header ${FRONTLINE_BRIDGING_HEADER}")
    endif()
    target_link_libraries(${target} PRIVATE frontline_bridge)
    add_dependencies(${target} frontline_game)
endfunction()
