# iPhone and iPad status

Hand-maintained. The iPhone and iPad port of the Apple TV app ([ROADMAP](../../ROADMAP.md#phase-3--iphone-ipad-and-apple-tv-m)), and the removal of iCloud from every app.

**Built (2026-11-21). Testing stop: the user runs the checklist below.** Compiled and linked for the iOS simulator, an iOS device (unsigned), the tvOS simulator and the Mac. Nothing has been run.

**The user's choices (2026-11-14):**
- Touch controls and Bluetooth controllers; no gyro.
- No iCloud in any app: saves stay on the device.
- tvOS keeps a copy of the memory card in its user defaults, against Caches being emptied.
- The disc is built into the app, as on tvOS (no import through Files).
- iPhone and iPad, one universal app, landscape only.
- One App ID for iOS and tvOS (`FRONTLINE_TV_BUNDLE_ID`).

**Who does what:** the user runs every test and does all signing.

## What was built

**I0: iCloud removed (all apps).**
- Gone:
  - `apps/shared/SaveSync.swift` and `Game.swift`'s `Saves`.
  - Play's wait for iCloud.
  - `PadBindings`' key-value mirror.
  - The tvOS entitlements file (the tvOS app now has none).
  - `FrontlineICloudContainer` in Info.plist, and the CloudKit link.
- `tools/tvos/port` and `port.json` no longer ask for iCloud. The paid team is still required, since a free team's installs stop after 7 days.
- The Mac never used iCloud: unchanged.

**I1: tvOS card backup** (`apps/ios/Sources/CardBackup.swift`).
- The card's change hook (`FrontlineSetCardChangeCallback`, fired after each write, format and delete) schedules a copy of every `.gci` in `Caches/Memory Card A` into the user default `memoryCardA`, as `[file name: data]`. It waits 0.5 s after the last change.
- Past 900 KB the copy is skipped and logged, since tvOS stops apps whose defaults pass about 1 MB.
- Before the game starts, a card folder with no `.gci` gets the copy back (`frontline: card backup: restored N save(s)`).

**I2: build.**
- The root `CMakeLists.txt` accepts `CMAKE_SYSTEM_NAME=iOS`: `FRONTLINE_IOS`, with `FRONTLINE_UIKIT` for what iOS and tvOS share (team, no tests, `apps/ios`, the audio session, the unsigned game image).
- Presets `ios`, `ios-sim` (and `-release`).
- `apps/ios/CMakeLists.txt`:
  - Builds `Frontline` (device family 1,2) on iOS and `FrontlineTV` on tvOS.
  - Each has its own asset catalog: `Assets-iOS.xcassets` is a flat 1024 px icon, the tvOS icon's layers stacked.
  - Info.plist's platform keys come from `FRONTLINE_PLIST_PLATFORM_KEYS`. On iOS:
    - landscape only, full screen, no status bar
    - `CADisableMinimumFrameDurationOnPhone`, Game Mode
    - extended gamepads
  - `Frontline-iOS.entitlements`: the increased memory limit, for the game's memory on phones. Extended virtual addressing was removed on 2026-11-21 at the user's request; it's paid-only per Apple's capability table. Whether the 4 GB address window (`address.cpp`) fits without it is a device check.
- `tools/tvos/port --platform ios` builds and installs the iOS app the same way (project in `build/port/xcode-ios`, result in `result-ios.json`).

**I3: the shell on iOS.**
- `GameViewController`:
  - A plain `UIViewController` on iOS (`GCEventViewController` stays tvOS's).
  - Hides the status bar and home indicator, and defers the screen edges' gestures.
  - Runs at 60 Hz, or up to the screen's rate (120) with Rewritten's high frame rate (not with Modern).
- `Settings.screenRate` and `screenAspect` come from the screen.
- The card is in `Application Support/Memory Card A`, which lasts.
- `SetupView` is one list on iOS, with a Touch section, an App section (pause in the background) and the frame rate on ProMotion screens. The picture size (overscan) and the Siri Remote hint are tvOS only.
- The audio session is SoloAmbient on iOS (it follows the silent switch) and Playback on tvOS.
- Exit Game works as on tvOS: B on the main menu, from a pad or the touch B button.

**I4: touch controls** (`apps/ios/Sources/TouchControls.swift`).
- **Stick:** the left 40% of the screen is a floating stick. In the menus it's the D-pad.
- **Look:** dragging elsewhere, off the buttons, looks through `FrontlineAddTouchLook`. That's new in `pad.cpp`: mouse look's accumulator, but the prompts stay the controller's. 0.0035 rad per point at sensitivity 1.
- **Buttons while playing:** Fire, Aim, Reload, Jump, Crouch, Use, Melee and Next are held actions (`FrontlineSetPadActions`). Pause, top right, is a one-shot press.
- **Buttons in the menus:** A, B and START.
- **Merging with pads:** the touch input goes into port 0 through `Controllers.touch`, merged with any gamepad there.
- **Visibility:** in Auto the controls hide once a gamepad is played, and the next touch brings them back. Settings: Auto/Always/Off, opacity, look speed, invert.

## Frontline Installer (2026-11-21)

**Superseded the same day** by the standalone installer: [installer-status.md](installer-status.md). The in-repository app described below was removed.

The user asked for installs without the Terminal: a small GUI helper, and a single `.app` on the Mac. Their choices:
- The Mac app keeps choosing its disc folder; the disc isn't built into it.
- The helper is a double-click app in the repository.
- It lives here, not in `tvos-GameTransferTool`.

**`Frontline Installer.app`** (repository root):
- It's kept as source: `Contents/Resources/Installer.swift`.
- Its program is a shell script (`Contents/MacOS/launcher`). macOS blocks a script app from reading the Documents folder outside its own bundle, without asking (found 2026-11-21: the first version, which compiled `tools/installer/`, wouldn't open).
- So the launcher reads only its own bundle. It compiles the source into a real app at `~/Library/Application Support/Frontline Installer/Frontline Installer.app`, ad hoc signed, the first time it's opened and whenever the source changes. Then it opens that app with `--repo <repository>`.
- That app asks for the Documents folder like any other. If access was refused, it says so and links to Privacy & Security.
- Compile errors go to `~/Library/Logs/Frontline Installer.log`, with an alert.
- Checked 2026-11-21: opened with `open`, it compiled, started and showed its window, and read the repository.
- The window, a SwiftUI app with no Xcode project:
  1. the disc (choose or drop)
  2. This Mac / iPhone or iPad / Apple TV, with the device from `devicectl list devices` (paired, physical, that platform)
  3. Check, and Build & Install
- It runs `tools/tvos/port … --json` and shows the stages, the build's progress, check results, errors with their fix, notes, and the output under Details. Cancel stops the script and everything it started.
- PATH adds Homebrew's folders and `/Applications/CMake.app/Contents/bin`, so CMake from cmake.org works without the Terminal.

**`tools/tvos/port --platform macos`:**
- Builds `FrontlineMac` (`RelWithDebInfo`, Xcode, ad hoc signed, game image inside) in `build/port/xcode-mac`, with no team needed.
- `install` copies it to `/Applications` (or `~/Applications`), quitting a running copy first. If the app's `disc` default isn't a disc, it points it at the prepared disc (`build/port/disc`), then opens the app.
- Verified 2026-11-21: `port build --platform macos` with the user's ISO succeeded, and the app's signature verifies. `install` and the installer window haven't been run.

**Checklist:**
- [ ] Double-click `Frontline Installer.app`. The first time, a notification says it's getting ready, and the window opens about 15 s later. If macOS asks about the Documents folder, allow it. The second open is instant.
- [ ] Choose the ISO (and try dropping it on the window).
- [ ] This Mac → Check: all green. Build & Install: the stages advance, and Frontline Rewritten opens from Applications, straight to its setup screen with the disc found.
- [ ] iPhone or iPad: your iPhone is in the device list. Build & Install installs and launches it, and a note gives the profile's expiry date.
- [ ] Apple TV: Guest Room is listed, and Build & Install still works as before.
- [ ] Cancel during a build stops it (no `xcodebuild` left in Activity Monitor).
- [ ] A failure (e.g. an iPhone unplugged during install) shows the message and its fix.

## Open items
- **Everything below is untested.** The button layout (`TouchControls.playing`) is a first guess: positions are points from the safe area's bottom right.
- `UIRequiresFullScreen` is deprecated from iOS 26. On iPad, the app could be put in a resizable window, and the picture would then be stretched to the window, with the screen's aspect.
- No rumble on the phone itself (only on a controller's haptics).
- The installer's binary is compiled on the user's Mac; a copy of the repository downloaded as a zip is quarantined, and macOS then asks before opening the app (Control-click → Open).

## Checklist for the user

Run from the repository root.

**1. Simulator** (no signing; the disc isn't bundled):

```sh
cmake --preset ios-sim
cmake --build --preset ios-sim
xcrun simctl boot "iPhone 17 Pro"
open -a Simulator
xcrun simctl install booted build/ios-sim/apps/ios/Debug-iphonesimulator/Frontline.app
SIMCTL_CHILD_FRONTLINE_DISC="$PWD/FrontlineDecompiled/orig/GMFE69/disc" xcrun simctl launch --console-pty booted com.vanillacoffeesoft.FrontlineRewritten
```

- [ ] The setup screen is one list, landscape; Play starts the game.
- [ ] In the menus the touch stick moves the selection, and A and B work. START works on the title screen.
- [ ] In a level: the stick moves, dragging on the right looks, and each button does what it says. Prompts show controller glyphs, not key names.
- [ ] Pause (top right) opens the pause menu; the menu buttons appear.
- [ ] B on the main menu offers Exit Game; Exit returns to the setup screen.
- [ ] Repeat on an iPad simulator (e.g. "iPad Pro 13-inch (M5)").

**2. Device:**

```sh
tools/tvos/port build --platform ios --disc "<your disc image>" --device <device identifier>
```

```sh
tools/tvos/port install --platform ios --device <device identifier> --launch
```

- [ ] The build signs (Xcode turns on Increased Memory Limit for the App ID; if it doesn't, turn it on at developer.apple.com).
- [ ] The game starts without extended virtual addressing: no `frontline: address window mmap` error in the console (`platform/src/address.cpp:167`). If it fails, the 4 GB reservation needs a smaller size or that capability back.
- [ ] It launches and a level plays on touch.
- [ ] Memory holds up: run it with `FRONTLINE_PERF=1` and the console (`xcrun devicectl device process launch --console --environment-variables '{"FRONTLINE_PERF":"1"}' --device <id> com.vanillacoffeesoft.FrontlineRewritten`); the perf line reports the available memory.
- [ ] A Bluetooth controller plays, the touch controls hide in Auto, and a touch brings them back.
- [ ] On a ProMotion iPhone or iPad Pro: Rewritten, Frame rate "Up to 120" runs smoothly.
- [ ] The silent switch mutes the game.

**3. Saves on iOS:**
- [ ] Save in the game, force-quit the app, relaunch: the save is there.

**4. Saves on tvOS** (rebuild the Apple TV app: `tools/tvos/port build`, then `install`):
- [ ] The build signs without iCloud set up.
- [ ] Save in the game. The console shows nothing unusual.
- [ ] The restore, on the tvOS simulator (the device's container can't be emptied by hand). Build and launch as in [phase3-status.md](phase3-status.md)'s simulator steps, save in the game, quit the app, then empty the card and relaunch:
  ```sh
  rm -rf "$(xcrun simctl get_app_container booted <bundle ID> data)/Library/Caches/Memory Card A"
  ```
  After Play, the console shows `frontline: card backup: restored N save(s)`, and the save loads.
- [ ] Controller bindings survive a relaunch.

**5. Mac regression:**
- [ ] Saves and controller bindings are unchanged.
