# The macOS app: a SwiftUI window with the game's Metal view, keyboard, mouse
# and game controller input, and a Settings window. Swift reaches the game
# through the shared bridge (apps/shared/Bridge), and compiles in the shared
# Swift sources (frontline_swift_app).

set(FRONTLINE_BUNDLE_ID "io.github.chocoguy.FrontlineRewritten"
    CACHE STRING "Bundle identifier for the macOS app")

add_executable(FrontlineMac MACOSX_BUNDLE
    Sources/Bindings.swift
    Sources/BindingsView.swift
    Sources/FrontlineApp.swift
    Sources/GameView.swift
    Sources/Input.swift
    Sources/Power.swift
    Sources/Renderer.swift
    Sources/Settings.swift
    Sources/SetupView.swift)

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
    XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS "${CMAKE_CURRENT_SOURCE_DIR}/FrontlineMac.entitlements"
    XCODE_GENERATE_SCHEME ON)

frontline_swift_app(FrontlineMac)

# The game image goes in the app's Frameworks folder (Bridge finds it there).
add_custom_command(TARGET FrontlineMac POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_BUNDLE_CONTENT_DIR:FrontlineMac>/Frameworks"
    COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:frontline_game>" "$<TARGET_BUNDLE_CONTENT_DIR:FrontlineMac>/Frameworks/"
    COMMAND codesign --force --sign - "$<TARGET_BUNDLE_CONTENT_DIR:FrontlineMac>/Frameworks/$<TARGET_FILE_NAME:frontline_game>"
    VERBATIM)

target_link_libraries(FrontlineMac PRIVATE
    "-framework AppKit"
    "-framework CoreHaptics"
    "-framework GameController"
    "-framework IOKit"
    "-framework SwiftUI"
    "-framework Metal"
    "-framework QuartzCore")
