# Frontline: Rewritten
## A native MOH Frontline port for macOS, iOS, and tvOS

![alt text](https://mangogravy.com/friconsmall.jpg)


This repo has **no game data**. You need your own copy of MOH: Frontline (USA, `GMFE69`).

## What it adds

The game runs in one of two game modes: Classic and Rewritten
Run Classic to play the game with it's original resolution and behavior
Run Rewritten to play the game with many QOL updates updates include:

- Mouse and Keyboard support for macOS
- FOV Slider
- 720p, 1080p, 1440p, and 4K native rendering using the Metal API
- Smarter enemy AI
- Widescreen support
- Longer distance render
- Per Pixel Lighting
- Shadows and dynamic lighting
- Mods
- Higher rate of fire for weapons
- Enemies drop items faster
- Crosshairs in all weapons
- M1 Garand can now be reloaded mid clip (Important!)


## Requirements

- An **Apple silicon** Mac
- macOS 27
- XCode 27
- tvOS: Apple TV 4k (3rd gen or newer) with tvOS 27
- iOS: An iPad or iPhone with iOS/iPadOS 27
- CMake 3.31 or later and Ninja and Python 3: `brew install cmake ninja`

## Install

Let's try the **Frontline Installer!** It's a simple app that takes your legally obtained disc image and builds the game from it: a .app for your Mac, or installed straight on your iPhone, iPad or Apple TV. It also keeps them up to date.

1. **Download Frontline Installer** from the [latest release](https://github.com/chocoguy/FrontlineRewritten/releases/latest), unzip it, and ***drag it to Applications Folder*** It's signed and notarized.
2. Have you dragged the .app package to the ***Applications Folder?*** 
3. **Install Xcode 27** from the App Store and open it once. Xcode builds the game; you won't need to use it.
4. **Open Frontline Installer and follow Setup.** It checks what this Mac needs and fixes what it can: it downloads CMake, and installs iOS or tvOS support in Xcode if you want those.
5. **Choose your disc:** a disc image of your own copy (ISO, GCM, CISO, GCZ, RVZ, WIA or TGC), or a disc extracted with Dolphin. You can drop it on the window.
6. **On Game, choose Install On…:**
   - **This Mac:** **Frontline Rewritten.app** goes in your Applications folder, set up to find your disc.
   - **An iPhone or iPad, or an Apple TV:** pick the device. It has to be paired with Xcode, and a paid Apple Developer team signed in to Xcode (Xcode → Settings → Accounts). See [iPhone and iPad](#iphone-and-ipad-manualdev-install) and [Apple TV](#apple-tv-manualdev-install) for pairing.

   The first build downloads the source and takes about 15 minutes; later ones are quicker. **Activity** shows what it's doing.

**Updates:** when a new version is released, **Updates** shows it with its notes. **Update All** rebuilds it once and reinstalls it everywhere you installed the game. A device that's off is finished later. When a release needs a new installer, it says so and links to it: download it and open it instead of the old one. Your disc, devices and build carry over.

**Before an install expires:** iPhone, iPad and Apple TV installs stop opening when their profile expires, after up to a year. The Game screen shows when each one stops working; press **Reinstall** before then. Your saves are kept.

Everything the installer downloads and builds (the source, the Xcode builds, CMake, logs) stays in one folder, `~/Library/Application Support/Frontline Rewritten Installer`, never in the app: **Setup → Storage** shows its size, frees the builds (several GB) or removes it all. It runs `tools/tvos/port`, which also works from the Terminal (below).

Saves go to the memory card in slot A: `~/Library/Application Support/Frontline Rewritten/Memory Card A`, one `.gci` file per save (the format of Dolphin's GCI folders and GameCube save tools)



## Controls


### Mouse and Keyboard

| Gameplay | Keys |
|---|---|
| Move / walk | W A S D / hold Left Shift |
| Look | Mouse |
| Fire / aim | Left button / hold right button |
| Jump / crouch | Space / C or Left Control |
| Action / reload / melee | F / R / V or middle button |
| Next / previous weapon | Wheel, or Q / E |
| Scope zoom | Wheel |
| Hint from HQ / frag counts | H / hold Tab |
| Pause / continue | Esc |

| Menus | Keys |
|---|---|
| D-pad | Arrow keys |
| A / B | Return / Esc or Delete |
| START | Space |
| X Y Z L R | X Y Z L R |


### Gamepad

It's just the standard FPS controls. It should automatically bind to whatever controller you are using, You can change the bindings in the launch settings screen


### Onscreen controls

Self explanatory, only shows up in iOS or iPadOS, Can also connect a gamepad and use that instead.





















## Developer Setup 
If you just want to play the game, you can ignore all that follows.

1. **Clone with the decomp submodule:**

   ```sh
   git clone --recursive <this repo>
   ```

   (or `git submodule update --init` in an existing clone)

2. **Build the GameCube reference** by following [FrontlineDecompiled/README.md](FrontlineDecompiled/README.md). All of it runs inside `FrontlineDecompiled/`, and it needs [Dolphin](https://dolphin-emu.org/) to extract the disc:
   1. In Dolphin, extract the whole disc into `orig/GMFE69/disc`.
   2. Convert the game's ELF to a DOL with decomp-toolkit (the README's step 2).
   3. Run `python3 configure.py --non-matching --map`, then `ninja`.

   This matters for two reasons:
   - It's the correctness check for every edit to the shared game code (`FrontlineDecompiled/tools/regress.py`).
   - It extracts a few binary pieces (the built-in font, the memory card icon) into `FrontlineDecompiled/build/GMFE69/include`, which the native build also compiles in.

   **Just building the apps, not editing the game code?** Skip this step. Run `tools/tvos/port prepare --disc <your disc image>` instead, which extracts the same pieces into `build/port/include`. Then add `-DFRONTLINE_EXTRACTED_DIR="$PWD/build/port/include"` when you configure a preset below.

3. **For the iOS and tvOS builds,** add those platforms in Xcode → Settings → Components.

## Building (Nerd Stuff)

**Xcode** (day-to-day work):

```sh
cmake --preset xcode
open build/xcode/FrontlineRewritten.xcodeproj
```

Run the `FrontlineMac` scheme. To build from the command line instead, use `cmake --build --preset xcode --target FrontlineMac`.

**For playing** (and for benchmarks and Instruments), build optimized. The `xcode` and `ninja` presets are Debug builds, which run the game code unoptimized; a Debug app run from Xcode also has Metal API validation on, which slows every draw.

```sh
cmake --preset release
cmake --build --preset release --target FrontlineMac
open "build/release/apps/macos/Frontline Rewritten.app"
```

In Xcode, `cmake --build --preset xcode-release` builds the same configuration (`RelWithDebInfo`). Set `FRONTLINE_PERF=1` to print a line a second saying where each frame's time went: frame rate, CPU, GPU, the waits, and draw and texture counts.


**Data check:** loads the converted file formats from your disc through the native loaders and validates them:

```sh
cmake --build --preset ninja --target frontline_datacheck
build/ninja/tests/datacheck/frontline_datacheck FrontlineDecompiled/orig/GMFE69/disc
```

**Headless run:** boots the game with no window or sound for a number of seconds (a smoke test), optionally saving the displayed frame each second (and, with `FRONTLINE_AUDIO_DUMP=<file.wav>`, the sound):

```sh
cmake --build --preset ninja --target frontline_headless
build/ninja/apps/headless/frontline_headless FrontlineDecompiled/orig/GMFE69/disc 10
```

**High frame rate test:** checks how frames drawn between the game's move the camera:

```sh
cmake --build --preset ninja --target frontline_hfrtest
build/ninja/tests/hfrtest/frontline_hfrtest
```

**OS test:** checks the platform's threads, interrupts, alarms and heaps against the console's rules:

```sh
cmake --build --preset ninja --target frontline_ostest
build/ninja/tests/ostest/frontline_ostest
```

**Native build report:** compiles the game code, collects every error and rewrites `docs/port/build-errors.md`:

```sh
python3 tools/build_report.py
```

Add `--sizes` to list the structs whose native layout differs from the GameCube's

**Frontline Installer** (`apps/installer`) builds with the Xcode preset; local builds are signed ad hoc, and build the releases of `port.json`'s major version:

```sh
cmake --build --preset xcode --target FrontlineInstaller
open "build/xcode/apps/installer/Debug/Frontline Installer.app"
```

`build/ninja/tests/installertest/frontline_installertest` (target `frontline_installertest`) checks its logic. Releases are made with `tools/release.sh`: see [docs/release.md](docs/release.md).

### Editing the game code (Nerd Stuff)

The game code lives only in the `FrontlineDecompiled` submodule, and the GameCube build has to keep working. The full rules are in [0004](docs/architecture/0004-compiler-rules.md). In short:

1. Make the edit in `FrontlineDecompiled/`. Put native-only code under `FRONTLINE_NATIVE`, preferably behind a helper in a header.
2. Rebuild the GameCube version (`ninja` in the submodule), and run `python3 tools/regress.py` there too. It must report 0 dropped and 0 missing.
3. Rerun `python3 tools/build_report.py` to see the native error count fall.
4. Commit in the submodule, push it, then commit the new submodule pointer here.

### Apple TV (Manual/Dev install)


**One-time setup:**
1. **Your team:** sign in to Xcode with your developer account (Xcode → Settings → Accounts). Your **Team ID** is on developer.apple.com → Account → Membership details.
2. **Nothing to set up on developer.apple.com:** the app uses no capabilities there, so Xcode registers the App ID (`com.frontlinerewritten.<your Team ID in lower case>`, or your own with `--bundle-id`) and makes the signing profiles itself when you build.
3. **Pair the Apple TV with Xcode:**
   - The Apple TV and the Mac have to be on the same network.
   - On the Apple TV, open Settings → Remotes and Devices → Remote App and Devices.
   - In Xcode, open Window → Devices and Simulators, pick the Apple TV and click Pair. Enter the code it shows.
   - If the Apple TV asks to turn on Developer Mode, accept.

**Or from Terminal**, in the repository root. First check that this Mac has everything:

```sh
tools/tvos/port check --disc "<your disc image>"
```

Then build and install. `xcrun devicectl list devices` shows your Apple TV's identifier:

```sh
tools/tvos/port build --disc "<your disc image>" --device <Apple TV identifier>
```

```sh
tools/tvos/port install --device <Apple TV identifier> --launch
```

`build` reads your disc once, in a few seconds: it checks that it's the right version of the game and extracts the pieces the app needs into `build/port`. Your image is never changed. It then builds the app and signs it with the paid team signed in to Xcode (`--team` picks another). Xcode registers the Apple TV and makes the signing profiles itself. The app is about 1.4 GB, so installing takes a few minutes over the network. `tools/tvos/port clean` removes the build and the extracted disc.

The tool and the script agree on [port.json](port.json), which describes what the port needs, and on the events `--json` prints.


**Updating, and expiry:** an app installed this way stops opening when its development profile expires, after up to a year. Build and install again to renew it, or to update it after pulling changes. Installing over the app keeps your saves.

**Troubleshooting:**
- **A provisioning error when building:** check that the paid team is signed in to Xcode, and that the bundle ID isn't another team's (pass a different `--bundle-id`).
- **The Apple TV isn't found:** wake it up, and check that it's paired in Xcode's Devices window.
- **The app quits right away:** run it with its console to see why:
  ```sh
  xcrun devicectl device process launch --console --device "<Apple TV name>" <bundle ID>
  ```
- **Crash reports from the Apple TV:**
  ```sh
  xcrun devicectl device info files --device "<Apple TV name>" --domain-type systemCrashLogs
  ```


**Simulator:** the `tvos-sim` preset builds for the Apple TV simulator without signing. It leaves the disc out:

```sh
cmake --preset tvos-sim
cmake --build --preset tvos-sim -- -sdk appletvsimulator
xcrun simctl install booted build/tvos-sim/apps/ios/Debug-appletvsimulator/FrontlineTV.app
SIMCTL_CHILD_FRONTLINE_DISC="$PWD/FrontlineDecompiled/orig/GMFE69/disc" xcrun simctl launch --console-pty booted <bundle ID>
```

### iPhone and iPad (Manual/Dev install)


**One-time setup:**
1. Sign in to Xcode with your developer account, as for the Apple TV.
2. Connect the iPhone or iPad to the Mac with a cable (or pair it over the network in Xcode's Devices window), and trust the Mac. Turn on **Developer Mode** when it asks (Settings → Privacy & Security → Developer Mode).


**Build and install** with [Frontline Installer](#install): choose your disc, pick iPhone or iPad and your device, and press Build & Install.

**Or from Terminal**, in the repository root. `xcrun devicectl list devices` shows your device's identifier:

```sh
tools/tvos/port check --platform ios --disc "<your disc image>"
```

```sh
tools/tvos/port build --platform ios --disc "<your disc image>" --device <device identifier>
```

```sh
tools/tvos/port install --platform ios --device <device identifier> --launch
```

The script is the Apple TV's, with the iOS SDK. The App ID is the same one, so the Apple TV and iPhone apps share a bundle ID.


**Simulator:** the `ios-sim` preset builds for the iPhone and iPad simulators without signing, and leaves the disc out:

```sh
cmake --preset ios-sim
cmake --build --preset ios-sim
xcrun simctl install booted build/ios-sim/apps/ios/Debug-iphonesimulator/Frontline.app
SIMCTL_CHILD_FRONTLINE_DISC="$PWD/FrontlineDecompiled/orig/GMFE69/disc" xcrun simctl launch --console-pty booted <bundle ID>
```


### Layout

| Path | Contents |
|---|---|
| `FrontlineDecompiled/` | Submodule: the decompiled game, the only copy of the game code |
| `game/` | CMake target that compiles the decomp's Game, Movie, EA System and EA Sound units with clang |
| `platform/` | The GameCube system libraries, natively: OS, DVD, VI, PAD, CARD, audio, GX |
| `apps/macos/` | SwiftUI + Metal app shell |
| `apps/ios/` | The iOS and tvOS app: UIKit scene, SwiftUI setup screen, game controllers, touch controls (iOS), the card backup (tvOS) |
| `apps/shared/` | What the apps share: the bridge to the game, settings, the display thread, controllers |
| `apps/headless/` | The game without a window, for smoke tests |
| `tests/` | `datacheck` (file formats on your disc), `ostest` (the OS layer), `aitest`, `hfrtest`, `limitertest` |
| `tools/` | `gen_sources.py` (unit list from the decomp), `build_report.py` (error work list) |
| `docs/architecture/` | Decisions on pointers, threads, byte order and compiler rules |
| `docs/port/` | Generated reports |



## Legal
Medal of Honor: Frontline is © Electronic Arts.

Shoutouts to Claude