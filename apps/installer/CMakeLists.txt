# Frontline Installer: the app players download (signed with a Developer ID and notarized by
# tools/release.sh). It builds the game from their disc at a GitHub release tag and installs it on
# the Mac and their devices, through tools/tvos/port. It doesn't update itself: a new major version is
# a new installer (docs/release.md). Built with the Xcode generator only (its asset catalog).

# The installer's version is the release it was built for: -DFRONTLINE_INSTALLER_VERSION from
# release.sh, which builds it only for a new major version (N.0.0), or port.json's for local builds.
# It builds the releases of its own major version.
set(FRONTLINE_INSTALLER_VERSION "" CACHE STRING "The release the installer is for (the tag without its v; empty: port.json's)")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/port.json")
set(_installer_version "${FRONTLINE_INSTALLER_VERSION}")
if(_installer_version STREQUAL "")
    file(READ "${PROJECT_SOURCE_DIR}/port.json" _port_json)
    string(JSON _installer_version GET "${_port_json}" version)
endif()
if(NOT _installer_version MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)(-[A-Za-z0-9.]+)?$")
    message(FATAL_ERROR "FRONTLINE_INSTALLER_VERSION ${_installer_version} isn't a version like 2.0.0")
endif()
set(FRONTLINE_INSTALLER_VERSION_FULL "${_installer_version}")
set(_installer_numeric "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${CMAKE_MATCH_3}")
math(EXPR _installer_build "${CMAKE_MATCH_1} * 10000 + ${CMAKE_MATCH_2} * 100 + ${CMAKE_MATCH_3}")

set(FRONTLINE_INSTALLER_BUNDLE_ID "com.vanillacoffeesoft.FrontlineInstaller"
    CACHE STRING "Bundle identifier for Frontline Installer")
# Ad hoc for local builds; tools/release.sh signs with "Developer ID Application".
set(FRONTLINE_INSTALLER_SIGN_IDENTITY "-" CACHE STRING "Code signing identity for Frontline Installer")

set(_sources
    Sources/AppModel.swift
    Sources/Doctor.swift
    Sources/InstallerApp.swift
    Sources/PortEvent.swift
    Sources/Releases.swift
    Sources/SemVer.swift
    Sources/Shell.swift
    Sources/Source.swift
    Sources/Views.swift
    Sources/Workspace.swift)
# Assets.xcassets holds AccentColor (#8AA6EE), the app's accent.
add_executable(FrontlineInstaller MACOSX_BUNDLE ${_sources} AppIcon.icns Assets.xcassets)
set_source_files_properties(AppIcon.icns Assets.xcassets PROPERTIES MACOSX_PACKAGE_LOCATION Resources)

set_target_properties(FrontlineInstaller PROPERTIES
    OUTPUT_NAME "Frontline Installer"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
    MACOSX_BUNDLE_GUI_IDENTIFIER "${FRONTLINE_INSTALLER_BUNDLE_ID}"
    MACOSX_BUNDLE_BUNDLE_NAME "Frontline Installer"
    MACOSX_BUNDLE_BUNDLE_VERSION "${_installer_build}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${_installer_numeric}"
    MACOSX_BUNDLE_ICON_FILE AppIcon
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${FRONTLINE_INSTALLER_BUNDLE_ID}"
    XCODE_ATTRIBUTE_SWIFT_VERSION 5.0
    XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_GLOBAL_ACCENT_COLOR_NAME AccentColor
    XCODE_ATTRIBUTE_CODE_SIGN_STYLE Manual
    XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "${FRONTLINE_INSTALLER_SIGN_IDENTITY}"
    XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${FRONTLINE_DEVELOPMENT_TEAM}"
    XCODE_ATTRIBUTE_ENABLE_HARDENED_RUNTIME YES
    XCODE_ATTRIBUTE_OTHER_CODE_SIGN_FLAGS "--timestamp"
    XCODE_GENERATE_SCHEME ON)
if(FRONTLINE_INSTALLER_SIGN_IDENTITY STREQUAL "-")
    # No timestamp server for ad hoc signatures.
    set_target_properties(FrontlineInstaller PROPERTIES XCODE_ATTRIBUTE_OTHER_CODE_SIGN_FLAGS "")
endif()

target_link_libraries(FrontlineInstaller PRIVATE
    "-framework AppKit"
    "-framework SwiftUI")
