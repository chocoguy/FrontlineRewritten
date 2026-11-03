#!/bin/sh
# Copies the game image into the app's Frameworks folder and signs it with the
# app's identity (Xcode's EXPANDED_CODE_SIGN_IDENTITY; ad hoc without one, as
# for the simulator). Runs as a build step of the app, before Xcode signs it;
# the folder comes from Xcode's build settings, which name the SDK's build
# folder (Debug-appletvos, Debug-appletvsimulator).
#   embed_game.sh <FrontlineGame.so>
set -e
frameworks="${TARGET_BUILD_DIR:?run this from Xcode}/${FRAMEWORKS_FOLDER_PATH:?}"
mkdir -p "$frameworks"
cp -f "$1" "$frameworks/"
codesign --force --timestamp=none --sign "${EXPANDED_CODE_SIGN_IDENTITY:--}" "$frameworks/$(basename "$1")"
