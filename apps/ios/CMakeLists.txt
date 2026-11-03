# The tvOS app: a UIKit scene with the SwiftUI setup screen, then the game's
# Metal view in a GCEventViewController, played with game controllers (and
# the Siri Remote in the menus). Swift reaches the game through the shared
# bridge (apps/shared/Bridge), and compiles in the shared Swift sources
# (frontline_swift_app). Laid out for iPhone and iPad to follow.

set(FRONTLINE_TV_BUNDLE_ID "com.vanillacoffeesoft.FrontlineRewritten"
    CACHE STRING "Bundle identifier for the tvOS app")

# iCloud (the saves, SaveSync; the controller bindings) needs the
# entitlements, which only a build signed with a team carries; without them
# CloudKit stops the app. Info.plist tells the app which container it has.
if(FRONTLINE_DEVELOPMENT_TEAM)
    set(FRONTLINE_ICLOUD_CONTAINER "iCloud.${FRONTLINE_TV_BUNDLE_ID}")
else()
    set(FRONTLINE_ICLOUD_CONTAINER "")
endif()

add_executable(FrontlineTV MACOSX_BUNDLE
    Sources/AppDelegate.swift
    Sources/Game.swift
    Sources/GameViewController.swift
    Sources/SetupView.swift)

set_target_properties(FrontlineTV PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
    MACOSX_BUNDLE_GUI_IDENTIFIER "${FRONTLINE_TV_BUNDLE_ID}"
    MACOSX_BUNDLE_BUNDLE_NAME "Frontline Rewritten"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${FRONTLINE_TV_BUNDLE_ID}"
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "3"
    # The team comes from FRONTLINE_DEVELOPMENT_TEAM (the root CMakeLists).
    XCODE_ATTRIBUTE_CODE_SIGN_STYLE Automatic
    XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS "${CMAKE_CURRENT_SOURCE_DIR}/FrontlineTV.entitlements"
    XCODE_GENERATE_SCHEME ON)

frontline_swift_app(FrontlineTV)

# The game image goes in the app's Frameworks folder (Bridge finds it there),
# signed with the app's identity: tvOS only loads code the app's team signed.
add_custom_command(TARGET FrontlineTV POST_BUILD
    COMMAND /bin/sh "${CMAKE_CURRENT_SOURCE_DIR}/embed_game.sh" "$<TARGET_FILE:frontline_game>"
    VERBATIM)

# The disc goes in the app: tvOS keeps nothing on disk for good. Build it with
# your own extracted disc; the app is for your own devices only (README).
# FRONTLINE_DISC_IN_BUNDLE=OFF leaves it out, for an app that installs
# quickly: the disc is then pushed once into the app's Caches, or (simulator)
# named by FRONTLINE_DISC.
set(FRONTLINE_DISC_DIR "${FRONTLINE_DECOMP_DIR}/orig/GMFE69/disc"
    CACHE PATH "The extracted GMFE69 disc (sys/ and files/) bundled into the tvOS app")
option(FRONTLINE_DISC_IN_BUNDLE "Copy the disc into the tvOS app" ON)
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
    add_custom_command(TARGET FrontlineTV POST_BUILD
        COMMAND /bin/sh "${CMAKE_CURRENT_SOURCE_DIR}/embed_disc.sh" "${FRONTLINE_DISC_DIR}"
        VERBATIM)
endif()

target_link_libraries(FrontlineTV PRIVATE
    "-framework AVKit"
    "-framework CloudKit"
    "-framework CoreHaptics"
    "-framework CoreMedia"
    "-framework GameController"
    "-framework Metal"
    "-framework QuartzCore"
    "-framework SwiftUI"
    "-framework UIKit")
