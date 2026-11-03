#!/bin/sh
# Copies the extracted disc (its sys/ and files/) into the app as Disc/,
# where the app finds it (Game.swift). Runs as a build step of the app, before
# Xcode signs it; only changed files are copied again. The folder comes from
# Xcode's build settings, which name the SDK's build folder.
#   embed_disc.sh <extracted disc folder>
set -e
disc="${TARGET_BUILD_DIR:?run this from Xcode}/${CONTENTS_FOLDER_PATH:?}/Disc"
mkdir -p "$disc"
rsync -a --delete --exclude .DS_Store "$1/sys" "$1/files" "$disc/"
