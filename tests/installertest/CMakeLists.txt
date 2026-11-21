# Checks the installer's logic without its UI: versions, the releases list, its saved state and the
# build script's events.
add_executable(frontline_installertest EXCLUDE_FROM_ALL main.swift ${FRONTLINE_INSTALLER_LOGIC_SOURCES})
set_target_properties(frontline_installertest PROPERTIES Swift_LANGUAGE_VERSION 5)
