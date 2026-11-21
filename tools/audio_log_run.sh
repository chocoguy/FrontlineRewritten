#!/bin/sh
# Plays the macOS app with the sound's health logged, to find what distorts
# it: a line a second from the mixer and the output (FRONTLINE_AUDIO_LOG),
# the frame times (FRONTLINE_PERF) and a recording of the mix as made
# (FRONTLINE_AUDIO_DUMP). Use the setup screen as usual, play about two
# minutes of the heavy action that distorts, then quit the app.
#   tools/audio_log_run.sh [build preset folder, default release]
# The log and recording go to build/logs/<time>/.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
app="$root/build/${1:-release}/apps/macos/Frontline Rewritten.app/Contents/MacOS/Frontline Rewritten"
out="$root/build/logs/$(date +%Y%m%d-%H%M%S)"
mkdir -p "$out"
echo "Logging to $out"
echo "Play about two minutes of heavy action, note roughly when it distorts, then quit the app."
FRONTLINE_AUDIO_LOG=1 FRONTLINE_PERF=1 FRONTLINE_AUDIO_DUMP="$out/mix.wav" "$app" 2> "$out/run.log" || true
echo "Done: $out/run.log ($(grep -c '^\[audio\]' "$out/run.log") seconds of sound logged), $out/mix.wav"
