# The UIKit app, for Apple TV (FrontlineTV) and for iPhone and iPad
# (Frontline): a UIKit scene with the SwiftUI setup screen, then the game's
# Metal view, played with game controllers (and the Siri Remote in the menus
# on tvOS, touch controls on iOS). Swift reaches the game through the shared
# bridge (apps/shared/Bridge), and compiles in the shared Swift sources
# (frontline_swift_app). The saves stay on the device.

# One App ID for both platforms.
set(FRONTLINE_TV_BUNDLE_ID "com.vanillacoffeesoft.FrontlineRewritten"
    CACHE STRING "Bundle identifier for the iOS and tvOS app")

if(FRONTLINE_TVOS)
    set(FRONTLINE_APP FrontlineTV)
    set(_family "3")
    # The layered tvOS icon (App Store size too) and Top Shelf images.
    set(_assets Assets.xcassets)
    set(_entitlements "")
    set(_sources Sources/CardBackup.swift)
    # Controllers drive the setup screen's focus; the game view turns that off
    # while playing (GameViewController). The Siri Remote is a micro gamepad.
    set(FRONTLINE_PLIST_PLATFORM_KEYS [=[
	<key>GCSupportsControllerUserInteraction</key>
	<true/>
	<key>GCSupportedGameControllers</key>
	<array>
		<dict>
			<key>ProfileName</key>
			<string>ExtendedGamepad</string>
		</dict>
		<dict>
			<key>ProfileName</key>
			<string>MicroGamepad</string>
		</dict>
	</array>]=])
else()
    set(FRONTLINE_APP Frontline)
    set(_family "1,2")
    # A flat 1024 px icon, the tvOS icon's layers stacked.
    set(_assets Assets-iOS.xcassets)
    set(_entitlements "${CMAKE_CURRENT_SOURCE_DIR}/Frontline-iOS.entitlements")
    set(_sources Sources/TouchControls.swift)
    # Landscape only, the whole screen, no status bar; ProMotion's 120 Hz; a
    # game for Game Mode; extended gamepads only.
    set(FRONTLINE_PLIST_PLATFORM_KEYS [=[
	<key>UISupportedInterfaceOrientations</key>
	<array>
		<string>UIInterfaceOrientationLandscapeLeft</string>
		<string>UIInterfaceOrientationLandscapeRight</string>
	</array>
	<key>UISupportedInterfaceOrientations~ipad</key>
	<array>
		<string>UIInterfaceOrientationLandscapeLeft</string>
		<string>UIInterfaceOrientationLandscapeRight</string>
	</array>
	<key>UIRequiresFullScreen</key>
	<true/>
	<key>UIStatusBarHidden</key>
	<true/>
	<key>CADisableMinimumFrameDurationOnPhone</key>
	<true/>
	<key>GCSupportsGameMode</key>
	<true/>
	<key>GCSupportedGameControllers</key>
	<array>
		<dict>
			<key>ProfileName</key>
			<string>ExtendedGamepad</string>
		</dict>
	</array>]=])
endif()

add_executable(${FRONTLINE_APP} MACOSX_BUNDLE
    Sources/AppDelegate.swift
    Sources/Game.swift
    Sources/GameViewController.swift
    Sources/SetupView.swift
    ${_sources}
    ${_assets})

set_target_properties(${FRONTLINE_APP} PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
    MACOSX_BUNDLE_GUI_IDENTIFIER "${FRONTLINE_TV_BUNDLE_ID}"
    MACOSX_BUNDLE_BUNDLE_NAME "Frontline Rewritten"
    MACOSX_BUNDLE_BUNDLE_VERSION "${FRONTLINE_BUILD_NUMBER}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${FRONTLINE_TV_BUNDLE_ID}"
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "${_family}"
    # As a resource, Xcode compiles the catalog into Assets.car.
    RESOURCE ${_assets}
    XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_APPICON_NAME "AppIcon"
    # The team comes from FRONTLINE_DEVELOPMENT_TEAM (the root CMakeLists).
    XCODE_ATTRIBUTE_CODE_SIGN_STYLE Automatic
    XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS "${_entitlements}"
    XCODE_GENERATE_SCHEME ON)

frontline_swift_app(${FRONTLINE_APP})

# The game image goes in the app's Frameworks folder (Bridge finds it there),
# signed with the app's identity: iOS and tvOS only load code the app's team
# signed.
add_custom_command(TARGET ${FRONTLINE_APP} POST_BUILD
    COMMAND /bin/sh "${CMAKE_CURRENT_SOURCE_DIR}/embed_game.sh" "$<TARGET_FILE:frontline_game>"
    VERBATIM)

# The disc goes in the app (tvOS keeps nothing on disk for good, and on iOS
# it saves an import step). Build it with your own extracted disc; the app is
# for your own devices only (README).
# FRONTLINE_DISC_IN_BUNDLE=OFF leaves it out, for an app that installs
# quickly: the disc is then pushed once into the app's Caches, or (simulator)
# named by FRONTLINE_DISC.
set(FRONTLINE_DISC_DIR "${FRONTLINE_DECOMP_DIR}/orig/GMFE69/disc"
    CACHE PATH "The extracted GMFE69 disc (sys/ and files/) bundled into the app")
option(FRONTLINE_DISC_IN_BUNDLE "Copy the disc into the app" ON)
if(FRONTLINE_DISC_IN_BUNDLE)
    set(_boot "${FRONTLINE_DISC_DIR}/sys/boot.bin")
    if(NOT EXISTS "${_boot}" OR NOT IS_DIRECTORY "${FRONTLINE_DISC_DIR}/files")
        message(FATAL_ERROR "${FRONTLINE_DISC_DIR} isn't an extracted disc (sys/boot.bin and files/). "
                            "Set FRONTLINE_DISC_DIR, or FRONTLINE_DISC_IN_BUNDLE=OFF.")
    endif()
    file(READ "${_boot}" _game_id LIMIT 6)
    if(NOT _game_id STREQUAL "GMFE69")
        message(FATAL_ERROR "${FRONTLINE_DISC_DIR} is ${_game_id}, not GMFE69 (Medal of Honor: Frontline, USA).")
    endif()
    add_custom_command(TARGET ${FRONTLINE_APP} POST_BUILD
        COMMAND /bin/sh "${CMAKE_CURRENT_SOURCE_DIR}/embed_disc.sh" "${FRONTLINE_DISC_DIR}"
        VERBATIM)
endif()

target_link_libraries(${FRONTLINE_APP} PRIVATE
    "-framework AVKit"
    "-framework CoreHaptics"
    "-framework CoreMedia"
    "-framework GameController"
    "-framework Metal"
    "-framework QuartzCore"
    "-framework SwiftUI"
    "-framework UIKit")
