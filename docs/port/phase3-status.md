# Phase 3 status

Hand-maintained. Phase 3 is the Apple TV port ([ROADMAP](../../ROADMAP.md#phase-3--iphone-ipad-and-apple-tv-m)).

**Complete (2026-11-02), by the user's decision:**
- The game runs and plays on Guest Room, an A15 Apple TV 4K: a level played through, saves going to iCloud.
- T7's measurement was closed without a run, since the game runs well as it is and newer Apple TVs will run it faster.
- The install steps are in the [README](../../README.md#apple-tv).

**Open items**, none needed for the port:
- **T7 measurement:** the `FRONTLINE_PERF` run (below) is kept, should Rewritten's tvOS defaults, MetalFX "4K (upscaled)" or a precompiled `.metallib` ever be wanted.
- **The rest of the T4 and T6 checklists:**
  - T4: split screen, a second controller kind, rumble, the Mac's controller sheet.
  - T6: restore after a reinstall, delete, offline.
- **The controller screen during play** on the TV, and **keyboard and mouse on tvOS** (T4 deviations).
- **The Mac on iCloud:** it keeps its Application Support card. Sharing the container needs its bundle ID moved to `com.vanillacoffeesoft.FrontlineRewritten`, with a migration of its UserDefaults.
- **iPhone and iPad:** the shell is shared and ready for them; touch controls and importing the disc through Files are the roadmap's plan.

**The plan (approved 2026-11-02):**
- One shared SwiftUI/UIKit shell (`apps/shared`, `apps/ios`). Only tvOS is finished this phase.
- Saves: CloudKit plus a local cache.
- Devices: A15 Apple TV 4K and newer.
- Gamepad rebinding.

**Milestones:**
- T0 device spikes
- T1 build
- T2 platform seams
- T3 shell
- T4 controllers
- T5 disc in the bundle
- T6 iCloud saves
- T7 A15 performance

**Who does what:** the user runs every test and does all signing. Each milestone stops with a checklist; the results go here.

**Simulator first (user, 2026-11-02):** every testing stop runs on the "Apple TV 4K (3rd generation)" simulator until the port is fully fleshed out. Then the whole port runs on Guest Room. The simulator doesn't enforce the device's address-space and memory limits or its code-signing rules for `dlopen`. So T0's device-only checks (`window`, `memory`, both bundle loads under real signing) stay open until that run, and the T0 fallbacks stay ready.

## Signing

**Done (2026-11-02):**
- **Team ID:** `USLHQ524FW`. It's passed to CMake as `SPIKE_TEAM` now, and later as `FRONTLINE_DEVELOPMENT_TEAM`.
- **App ID:** explicit, `com.vanillacoffeesoft.FrontlineRewritten`, with iCloud (CloudKit) on. This is the tvOS app's bundle ID.
- **iCloud container:** `iCloud.com.vanillacoffeesoft.FrontlineRewritten`. It is assigned to the App ID through iCloud's Configure button.

**Notes:**
- The Mac app keeps `io.github.chocoguy.FrontlineRewritten` (`FRONTLINE_BUNDLE_ID`) for now. Changing it would reset the Mac's UserDefaults (settings and key bindings). Moving the Mac to the new ID, with a defaults migration, is part of the Mac iCloud follow-up after T6.
- The spike's own ID, `com.vanillacoffeesoft.FrontlineSpike`, is registered by automatic signing the first time it's built for a device.

**Before the bare-metal run:** pair Guest Room with Xcode.
- On the TV: Settings → Remotes and Devices → Remote App and Devices.
- In Xcode: Window → Devices and Simulators.

## T0: device spikes

**What it was:** `tools/spikes/tvos/` held a throwaway tvOS app, deleted on 2026-11-02 once the app itself ran on Guest Room. It did what the platform layer does and printed one `spike: PASS|FAIL <check>: <detail>` line per check. The commands below are kept for the record.

| Check | What the port relies on it for |
|---|---|
| `memory` | the app's memory budget at launch (`os_proc_available_memory`) |
| `window` | the 4 GB `PROT_NONE` reservation (`address.cpp` `ArenaInit`). On failure, it also prints the largest size that worked. |
| `arena` | 256 MB made read-write and every page touched; the memory budget before and after |
| `mincore` | the reboot snapshot's page scan (`reboot.cpp` `SaveArena`) |
| `remap` | `MAP_FIXED` fresh pages in place (`RestoreArena`) |
| `stack` | a thread on a 2 MB stack inside the arena (`thread.cpp`) |
| `low4g` | `__PAGEZERO` covers the low 4 GB, so `NativePtrToAddrSlow`'s integer case is safe |
| `dynamic_lookup` (3 lines) | a bundle linked like `FrontlineGame.so` loads and calls the executable; dyld's image name equals `realpath` (the reboot finds the image that way); its writable segments can be rewritten in place |
| `bundle_loader` (3 lines) | the same for a bundle linked with `-bundle_loader`, the fallback link |

**Already known from building it (2026-11-02):**
- Both bundles link for device and simulator.
- The linker warns: "-undefined dynamic_lookup is deprecated on tvOS". It's a warning, not an error.

**From the first simulator run (2026-11-02):** it trapped at launch in UIKit's `_UIApplicationEvaluateRuntimeIssueForNoSceneLifecycleAdoption`. Apps built with the tvOS 27 SDK must use the scene lifecycle (`UIApplicationSceneManifest` and a `UIWindowSceneDelegate`). The spike now has both. In T3, the SwiftUI `App` adopts scenes by itself, and `GameViewController` gets its window from the scene.

### Checklist for the user

Run from the repository root.

**Simulator** (no signing needed):
```bash
cmake -S tools/spikes/tvos -B build/spike-tvos -G Xcode
cmake --build build/spike-tvos --config Debug -- -sdk appletvsimulator
xcrun simctl boot BEF1BE80-352F-45F1-9252-ACEBCB79CD10
xcrun simctl install BEF1BE80-352F-45F1-9252-ACEBCB79CD10 build/spike-tvos/Debug-appletvsimulator/FrontlineSpike.app
xcrun simctl launch --console-pty BEF1BE80-352F-45F1-9252-ACEBCB79CD10 com.vanillacoffeesoft.FrontlineSpike
```
(That UDID is the "Tv 3rd gen" simulator, an Apple TV 4K (3rd generation).)

**Guest Room** (deferred to the bare-metal run):
```bash
cmake -S tools/spikes/tvos -B build/spike-tvos -G Xcode -DSPIKE_TEAM=USLHQ524FW
cmake --build build/spike-tvos --config Debug -- -sdk appletvos -allowProvisioningUpdates
xcrun devicectl device install app --device "Guest Room" build/spike-tvos/Debug-appletvos/FrontlineSpike.app
xcrun devicectl device process launch --console --device "Guest Room" com.vanillacoffeesoft.FrontlineSpike
```
Alternatively, open `build/spike-tvos/FrontlineSpike.xcodeproj`, pick the FrontlineSpike scheme and Guest Room, and Run. The lines then show in Xcode's console.

**Report back:** every `spike:` line from the simulator run. If the app doesn't install or launch, send the error text instead.

**What the results decide.** Simulator results settle these only provisionally. T1 builds on the simulator's answer, and the bare-metal run confirms it or switches to the fallback.
- **`window` fails on the device:** on tvOS the window becomes the arena plus its guard (ADR 0001 revision).
- **`dynamic_lookup` fails but `bundle_loader` passes:** the game image links with `-bundle_loader` against a stub executable.
- **Both fail:** the platform layer becomes `FrontlinePlatform.framework`.
- **The dyld name differs from `realpath`:** `LoadGame` and the reboot match images by header instead of by name.

### Results

**Simulator, "Tv 3rd gen" (tvOS 27), 2026-11-02: everything passes.**

| Check | Result |
|---|---|
| `memory` | 0 MB. The simulator has no memory limit, so `os_proc_available_memory` returns 0. This waits for Guest Room. |
| `window` | 4 GB reserved at `0x7000000000` |
| `arena` | 256 MB committed and touched |
| `mincore` | 16,380 of 16,380 pages resident |
| `remap` | in place, pages zero |
| `stack` | thread on an arena stack ran |
| `low4g` | `__PAGEZERO` is `0x100000000`; the first mapping is at `0x100fbc000`, just above 4 GB |
| `dynamic_lookup` | loads and calls the host; dyld's name equals `realpath`; 16 KB of writable segments rewritten |
| `bundle_loader` | the same |

**Provisional decisions for T1** (Guest Room confirms or overturns them):
- The address window stays 4 GB.
- The game image keeps `-undefined dynamic_lookup`, as on the Mac. `-bundle_loader` is the tested fallback.
- `LoadGame` and the reboot keep matching the image by name.

## T1: build system

**Done (2026-11-02).** There's no testing stop here: T1 builds no app yet, only the two pieces of the port that run on the TV.

**Presets:**

| Preset | Build folder | What it is |
|---|---|---|
| `tvos` | `build/tvos` | device, Xcode generator, `CMAKE_SYSTEM_NAME=tvOS`, SDK `appletvos` |
| `tvos-sim` | `build/tvos-sim` | simulator, SDK `appletvsimulator` |

- Build presets: `tvos`, `tvos-release`, `tvos-sim` and `tvos-sim-release` (Debug and RelWithDebInfo).
- Device builds take `-DFRONTLINE_DEVELOPMENT_TEAM=USLHQ524FW` on the configure line. CMake hands it to Xcode as `DEVELOPMENT_TEAM`.

```bash
cmake --preset tvos-sim
cmake --build --preset tvos-sim -- -sdk appletvsimulator
```

**What changed:**
- **Root `CMakeLists.txt`:**
  - `FRONTLINE_TVOS` comes from `CMAKE_SYSTEM_NAME`.
  - The tests and `apps/headless` build only on the Mac.
  - On tvOS no app is added until T3.
- **`game/CMakeLists.txt`:** on tvOS, the game image isn't signed by its own target. The app signs it with its own identity when it embeds it, as the spike does.
- **`platform/src/game_hooks.cpp`:** the `execv` relaunch (and its `/tmp` file) is macOS-only. On tvOS, if the in-place reboot isn't possible, the game exits with a message.
- **`platform/src/audio_out.cpp`:** the output unit is RemoteIO off macOS, so it compiles. The audio session (category, interruptions, route changes) is T2.

**Build results:**
- `frontline_platform` and `FrontlineGame.so` build for the device (`platform TVOS`) and the simulator (`TVOSSIMULATOR`), minos 27.0. The game image is about 3.0 MB, the same as the Mac's.
- The only new warning: "-undefined dynamic_lookup is deprecated on tvOS", as the spike showed.
- `MTLMathModeSafe` (`mesh_draw.mm`) and `CGColorCreateGenericGray` (`key_icons.mm`) compile on the tvOS SDK, so neither needs replacing.
- The Mac's `ninja` and `release` presets still build: the tests, the headless runner and the app.

## T2: platform seams

**Built (2026-11-02).** It's checked at the T3 testing stop, since there's no app to hear or see it in before then.

**Audio** (`platform/src/audio_session.mm`, tvOS only, plus `audio_out.cpp`):
- RemoteIO plays the 32 kHz float stream as before; the session converts it to the TV's rate.
- **Session:** category `AVAudioSessionCategoryPlayback`, activated before the unit opens. If it can't be activated, the game runs without sound and says why.
- **Interruption begins** (Siri, another app's audio): the unit stops.
- **Interruption ends:** the session is reactivated and the unit restarts, whatever the interruption's ShouldResume option says.
- **Route change** (new or removed device, override, route configuration change): the unit restarts.
- **Every restart** drops what was queued before it, so stale sound never plays late.
- **Media services reset:** the unit is made anew.
- **Threads:** the events come from the session's notification threads. A recursive lock covers them, since opening the unit may post one on the opening thread, and an event leaves alone a unit that is still being opened.

**Working folder:** on tvOS, `LoadGame` moves the working folder to the app's Caches (`$HOME/Library/Caches`) before the game image loads. That gives every relative path the platform writes a home:
- `FRONTLINE_AUDIO_DUMP`
- `FRONTLINE_DISPLAY_DUMP`
- `FRONTLINE_HFR_DUMP`
- `FRONTLINE_GX_SNAP_DIR`, whose default is `.`
- `FRONTLINE_NATIVE_VERIFY_DUMP`

Pull them with `xcrun simctl get_app_container <udid> <bundle id> data` (simulator) or `devicectl device copy from --domain-type appDataContainer` (device).

**Address window:** unchanged, as T0 decided.

**Build results:**
- `tvos`, `tvos-sim`, `ninja` and `release` all build.
- The session code is only in the tvOS library.

**For the T3 checklist:**
- sound plays at the menus;
- Siri or another app's audio interrupts it, and it comes back;
- AirPods connected and disconnected while playing (on the simulator, change the output device);
- `FRONTLINE_AUDIO_DUMP=dump.wav` writes into Caches.

## T3: shared shell and the tvOS app

**Built (2026-11-02). Testing stop: the user runs the checklist below.**

**`apps/shared`** (used by both apps):
- `Bridge/`, moved from `apps/macos` with its API unchanged, plus `FrontlineSetPictureSize`.
- `SettingsKeys.swift`: the settings keys and choices, plus `Settings.apply` and `applyLive`, which push them to the game. Each app adds its own `screenRate` and `screenAspect`.
- `Presenter.swift`: the `CAMetalDisplayLink` display thread, moved unchanged.
- `Controllers.swift`: gamepad reading and rumble, moved from the Mac's `Input.swift`, plus:
  - the Siri Remote for the menus (`remoteMenus`), used only when no gamepad is connected: arrows or touch surface = D-pad, select = A, Back = B, Play/Pause = START. This came forward from T4 so the menus can be used without a controller.
  - a latch for the menu buttons (a gamepad's Menu, the remote's Back and Play/Pause), so a press shorter than a frame still reaches the game.
- `CMakeLists.txt`: the `frontline_bridge` library and `frontline_swift_app()`, which gives an app the shared sources, the bridging header and Swift 6.

**The Mac app:** same behavior. It now uses the shared code, `Input.swift` keeps only the keyboard and mouse, and a very quick START press can no longer be missed.

**The tvOS app (`apps/ios`, target `FrontlineTV`):**
- **App structure:** a UIKit scene, not a SwiftUI `App` as planned. `GameViewController` is a `GCEventViewController`, and it only keeps controller buttons from UIKit (Menu would leave the app) when it's the window's root, so Play swaps it in for the setup screen's `UIHostingController`.
- **Setup screen:** a focus-engine `Form`:
  - Play is first.
  - Mode is a segmented picker.
  - Rewritten's options are hidden in Classic. The field of view is a list in 5° steps, and there's no high frame rate.
  - Picture size (Full screen, 95%, 90%) is under Screen, in both modes.
  - It says when no controller is connected.
- **Game view:**
  - The drawable is sized in the screen's pixels.
  - `controllerUserInteractionEnabled = false`.
  - Input is read at 60 Hz and the presenter runs at 60.
  - The idle timer is off.
  - `AVDisplayManager` asks a TV with Match Frame Rate on for 59.94 Hz SDR.
- **Leaving the app** (`sceneWillResignActive`) pauses the game if it's being played.
- **Picture size:** `frontline::SetPictureSize` (`display.mm`) fits the picture in that share of the screen, centered.
- **Disc:** `FRONTLINE_DISC`, then `Caches/Disc`, then a `Disc` folder in the app (T5).
- **Memory card:** `Caches/Memory Card A`. iCloud comes in T6.
- **Info.plist:** the scene manifest, controller keys (`GCSupportsControllerUserInteraction`, extended and micro gamepads), arm64 and metal, dark appearance.
- **Entitlements** (`FrontlineTV.entitlements`): the iCloud container `iCloud.$(PRODUCT_BUNDLE_IDENTIFIER)`, CloudKit, and key-value storage. A simulator build without a team carries none of them; nothing uses iCloud before T6.
- **The game image:** `embed_game.sh` copies `FrontlineGame.so` into `FrontlineTV.app/Frameworks` and signs it with the app's identity (ad hoc on the simulator). It gets the folder from Xcode's build settings.

**Build results:**
- `tvos-sim` builds and signs ad hoc.
- `tvos` builds with `CODE_SIGNING_ALLOWED=NO`. Signing it is the user's step; automatic signing creates the profiles.
- The Mac's `ninja`, `release` and `xcode` presets build.
- There are no new Swift warnings.

### Checklist for the user (simulator)

Run from the repository root.

```bash
cmake --preset tvos-sim
cmake --build --preset tvos-sim -- -sdk appletvsimulator
xcrun simctl boot BEF1BE80-352F-45F1-9252-ACEBCB79CD10
xcrun simctl install BEF1BE80-352F-45F1-9252-ACEBCB79CD10 build/tvos-sim/apps/ios/Debug-appletvsimulator/FrontlineTV.app
SIMCTL_CHILD_FRONTLINE_DISC="$PWD/FrontlineDecompiled/orig/GMFE69/disc" \
  xcrun simctl launch --console-pty BEF1BE80-352F-45F1-9252-ACEBCB79CD10 com.vanillacoffeesoft.FrontlineRewritten
```

`SIMCTL_CHILD_<name>` passes `<name>` to the app, so any `FRONTLINE_*` variable works the same way, for example `SIMCTL_CHILD_FRONTLINE_PERF=1`. Dumps land in the app's Caches:

```bash
open "$(xcrun simctl get_app_container BEF1BE80-352F-45F1-9252-ACEBCB79CD10 com.vanillacoffeesoft.FrontlineRewritten data)/Library/Caches"
```

**Input on the simulator:** a controller connected to the Mac (Bluetooth or USB) should reach the simulator. The simulator's Apple TV Remote window (Window → Apple TV Remote) works the setup screen; whether it also shows up to the game as a Siri Remote is one of the checks.

**Check:**
1. **Setup screen:** focus moves with the remote or controller; Classic and Rewritten switch; Rewritten's options appear and are remembered after a relaunch.
2. **Play:** the intro movies play with sound, then the title and the main menu.
3. **Menus with a controller:** A, B, the D-pad and Menu (START) work, and Menu doesn't leave the app.
4. **Menus with only the remote** (no controller connected): arrows, select (A), Back (B), Play/Pause (START).
5. **Picture:** Classic 4:3 with black bars. Rewritten with widescreen fills 16:9 in levels. Picture size 90% shows a black border all around.
6. **Leaving the app** (Home on the remote, or Shift-Command-H) while in a level, then coming back: the game is paused and sound comes back.
7. **Audio (from T2):** sound at the menus. `SIMCTL_CHILD_FRONTLINE_AUDIO_DUMP=dump.wav` writes `dump.wav` into Caches.
8. **D-Day:** start it and play a couple of minutes (needs a controller); then Abort Mission, which reboots in place, and replay.
9. **Mac regression** (the shell code moved):
   - Build and open the Mac app: `cmake --build --preset release`, then `open "build/release/apps/macos/Frontline Rewritten.app"`.
   - It boots to the menus.
   - The keyboard, mouse and a controller still work.
   - The Settings window still changes the field of view while playing.

**Report back:** what passed or failed, the console output around any problem, and a screenshot if the picture looks wrong (Command-S in the simulator).

## T5: the disc in the app

**Built (2026-11-02), ahead of T4 so the app can be tested on Guest Room.**

- **Options:**
  - `FRONTLINE_DISC_DIR` names the disc. The default is `FrontlineDecompiled/orig/GMFE69/disc`.
  - `FRONTLINE_DISC_IN_BUNDLE` is ON by default. The `tvos-sim` preset turns it OFF, because the simulator reads the disc from the Mac through `FRONTLINE_DISC`.
- **Configure-time check:** `sys/boot.bin` and `files/` exist, and `boot.bin` starts with `GMFE69`.
- **The copy:** `apps/ios/embed_disc.sh` runs after the game image is embedded and before Xcode signs the app. It `rsync`s `sys/` and `files/` into `FrontlineTV.app/Disc`, skipping `.DS_Store`.
  - The first build copies all 515 files (1.3 GB); an unchanged rebuild takes about a second.
  - Xcode's code signing still hashes the whole disc on each signed build.
- **How the app finds it:** `FRONTLINE_DISC`, then `Caches/Disc`, then the app's `Disc` (`Game.swift`).
- **Size:** the app is 1.4 GB, well under tvOS's 4 GB app limit.
- **Install time on Guest Room:** not measured yet; it's on the device checklist.
- **Personal use only:** the README says to keep the app on your own devices and not upload it to App Store Connect or TestFlight.

### Device checklist (Guest Room)

1. **Pair:** on the TV, Settings → Remotes and Devices → Remote App and Devices; in Xcode, Window → Devices and Simulators → Pair. Accept Developer Mode if asked.
2. **The T0 spike, signed** (it settles the device-only T0 questions):
   ```bash
   cmake -S tools/spikes/tvos -B build/spike-tvos -G Xcode -DSPIKE_TEAM=USLHQ524FW
   cmake --build build/spike-tvos --config Debug -- -sdk appletvos -allowProvisioningUpdates
   xcrun devicectl device install app --device "Guest Room" build/spike-tvos/Debug-appletvos/FrontlineSpike.app
   xcrun devicectl device process launch --console --device "Guest Room" com.vanillacoffeesoft.FrontlineSpike
   ```
3. **The app, signed, with the disc:**
   ```bash
   cmake --preset tvos -DFRONTLINE_DEVELOPMENT_TEAM=USLHQ524FW
   cmake --build --preset tvos -- -sdk appletvos -allowProvisioningUpdates
   time xcrun devicectl device install app --device "Guest Room" build/tvos/apps/ios/Debug-appletvos/FrontlineTV.app
   xcrun devicectl device process launch --console --device "Guest Room" com.vanillacoffeesoft.FrontlineRewritten
   ```
   - For full speed, use `--preset tvos-release`; the app is then in `RelWithDebInfo-appletvos`.
   - `FRONTLINE_*` variables go in with `--environment-variables '{"FRONTLINE_PERF":"1"}'`.
4. **Then the T3 checklist on the TV:**
   - the real Siri Remote and a Bluetooth controller (paired in the TV's Settings);
   - the TV button to leave the app and come back;
   - Abort Mission, which reboots in place on real hardware.

**Report back:**
- the spike's `spike:` lines;
- the install time;
- what passed or failed, with the console text around any problem.

### Device results (Guest Room)

**2026-11-02, from the user:** the signed app with the bundled disc loads on Guest Room, and a whole level played through with a gamepad. That settles the device-only T0 questions on real hardware:
- the 4 GB address window is allowed;
- a team-signed `-undefined dynamic_lookup` game image loads;
- the game fits tvOS's memory budget.

The spike's exact numbers and the install time weren't reported.

## T4: controllers on the TV, and gamepad rebinding

**Built (2026-11-02). Testing stop: the user runs the checklist below.**

**Ports** (`apps/shared/Controllers.swift`, `separatePorts`, on for tvOS):
- Each gamepad gets its own GameCube port, 0–3, in the order it connected, and keeps it until it disconnects. A new gamepad takes the first free port.
- The controller's player light (`playerIndex`) is set to match.
- Port 0 always reads as connected, because the remote drives it. Ports 1–3 read as unplugged without a gamepad, as on the console.
- The Siri Remote works port 0's menus only while no gamepad is connected.
- Rumble runs per port, on that port's controller.
- **The Mac** still merges every gamepad into port 0, with the keyboard and mouse.

**Rebinding** (`apps/shared/PadBindings.swift`):
- **What a button can do:** be a GameCube button (A B X Y Z L R START, the D-pad), a game action, or nothing.
  - The actions: melee, reload, action, jump, crouch, next and previous weapon, center view, hint, frag counts, pause.
  - L and R stay analog: a trigger bound to them presses as far as it's pulled and clicks the digital button at the end. Any other button presses them all the way.
- **Buttons covered:** the face buttons, both shoulders, both triggers, both stick clicks, Menu, Options and the D-pad, where the controller has them.
- **Sticks:** swap, and invert up/down per stick.
- **Defaults** are today's mapping: shoulders → Z, triggers → L/R, Menu → START. The stick clicks and Options do nothing by default.
- **Storage:**
  - one profile per controller kind (`GCController.productCategory`), in the `padBindings` default as JSON;
  - a kind with no profile plays with the defaults;
  - on tvOS it's mirrored to iCloud key-value storage (`syncWithICloud` at launch, and whenever another device changes it).
- **Applies at once,** also while the game runs.

**Bound actions reach the game** through a new platform call, `SetPadActions` (`pad.cpp`, bridged as `FrontlineSetPadActions`). They add to the keyboard's actions, but count as controller use, so on-screen prompts stay the controller's. Sending them through `SetKeyboardMouse` would have switched prompts to key names.

**Quick presses:** every gamepad button's press is now latched until the next read (it was only Menu before), so a press shorter than a frame still counts.

**The controller screen** (`apps/shared/PadBindingsView.swift`):
- A row per button, labeled with the controller's own name and glyph (`localizedName`, `sfSymbolsName`), with a picker of what it does. Then the stick options, and Reset to Defaults.
- It shows the kind of controller connected; with two kinds connected, there's a picker for which one.
- **tvOS:** Controls → Controller buttons on the setup screen.
- **Mac:** Settings → Game controller → Controller Buttons…, in a sheet.

**Deviations from the plan:**
- **Pickers instead of "press the new button" capture.** On the TV, A and B also move around the screen, so capturing them would fight the focus engine.
- **No controller screen during play on the TV yet.** There's no pause overlay to open it from; it's reachable from the setup screen.
- **No keyboard and mouse on tvOS yet.** They were low priority in the plan.

**Interplay with tvOS:** the system's per-app controller remapping (Settings → General → Game Controllers) applies before the app's. The controller screen then lists the buttons as the system remapped them.

**Build results:** `tvos-sim`, `tvos` (unsigned), `ninja` and `release` all build, with no Swift warnings.

### Checklist for the user (Guest Room or the simulator)

Build and install as in the T5 device checklist.

1. **Defaults unchanged:** a gamepad plays exactly as before, with prompts showing GameCube buttons.
2. **Rebinding:**
   - On the setup screen, open Controls → Controller buttons.
   - Set Right Stick Button → Melee, and Options (View on Xbox, Create on DualSense) → Reload.
   - Play D-Day: melee with the stick click, reload with Options. Prompts still show controller buttons, not keys.
3. **Sticks:** turn on "Invert the C stick's up and down"; looking up and down flips.
4. **Reset to Defaults** puts everything back.
5. **Persistence:** quit the app (double-click TV → swipe it away), relaunch, and the bindings are still there.
6. **A second kind of controller** (DualSense and Xbox, if you have both): each keeps its own bindings, and the screen offers a picker between them.
7. **Two controllers, split screen** (Classic's multiplayer): each controller drives its own player, and the lights show player 1 and player 2. Unplug or turn off controller 2, and its player shows as disconnected.
8. **Siri Remote:** with no gamepad connected, the remote still works the menus.
9. **Rumble** on the controller that's being hit, in split screen too.
10. **Mac:** Settings → Game controller → Controller Buttons… shows the same screen, and a binding changed there works in the game at once.

**Report back:** what passed or failed, and the controller models used.

**T4 follow-up (2026-11-02):** every list picker on the TV opened a blank pushed screen (tvOS's default `navigationLink` picker style). The setup screen and the controller screen now use `.pickerStyle(.menu)`, and the user confirmed it works.

## T6: iCloud saves

**Built (2026-11-02). Testing stop: the user runs the checklist below.**

**Platform hook:** `frontline::SetCardChangeCallback` (`card.cpp`, bridged as `FrontlineSetCardChangeCallback`) reports each card change on the game thread:
- after a `.gci` is written in full (the rename in `WriteGci`);
- after one is deleted (`CARDDelete`, `CARDFormat`).

**`apps/shared/SaveSync.swift`** (an actor), in the CloudKit private database:
- **Records:** one `MemoryCardFile` per `.gci`, named after the file, in a custom zone, `MemoryCard`. Each record holds:
  - `file` (an asset);
  - `modified` (a date);
  - `sha256`.

  It fetches with zone changes, so no CloudKit schema or index setup is needed. The development schema is created by the first upload.
- **`prepare`** runs at launch:
  1. checks the iCloud account;
  2. creates the zone;
  3. fetches every record;
  4. reconciles them with the card folder against a manifest (`Memory Card A Sync.json`, next to the folder), which holds each file's SHA-256 at the last sync.
- **The reconcile rules:**
  - changed on one side: that side wins;
  - changed on both: the newer `modified` wins, and the other copy goes to `Memory Card A Conflicts/`, which the card never reads;
  - deleted here: deleted in iCloud;
  - deleted elsewhere: the local copy moves to the conflicts folder rather than being deleted.
- **During play:** each change is queued and sent in turn. The file is read when its turn comes, so the latest version goes up. A missing file deletes the record. Whatever fails, such as being offline, is put right at the next launch's `prepare`.
- **The card folder belongs to the game once it starts:** `beginGame` stops all local writes, and from then on only uploads happen.

**The tvOS app:**
- `Saves.prepare()` runs at launch.
- **Play** waits for it, 5 seconds at most; the button says "Getting your saves from iCloud…" meanwhile. Then it hands the card to the game and sets the change callback.
- **iCloud is on only in builds signed with a team.** CMake writes the container into Info.plist (`FrontlineICloudContainer`) only when `FRONTLINE_DEVELOPMENT_TEAM` is set. Without the entitlements, CloudKit would stop the app, so an unsigned simulator build keeps its saves in Caches and leaves the controller bindings' iCloud mirror off too.

**Logs:** every step prints `frontline: saves: …` to the console.

**The Mac:** keeps its Application Support card. Moving it to the same container, which needs its bundle ID to change, is a follow-up; the record format is shared already.

**Build results:**
- `tvos-sim` has no container in its Info.plist.
- `tvos` (team set) names `iCloud.com.vanillacoffeesoft.FrontlineRewritten`.
- `ninja` and `release` build.
- There are no Swift warnings.

**Fix (2026-11-02): the first save crashed the app.**
- **Cause:** the crash report from Guest Room shows the trap in `swift_task_isCurrentExecutor`, on the game's thread, under `WriteGci` → the card callback. The callback was a closure written inside a `@MainActor` function, so Swift 6 made it the main actor's and checked that when the game's thread called it.
- **Fix:** it's now a top-level function with no actor (`cardChanged` in `Game.swift`).
- **Rule for the port:** a callback the platform calls from a game thread must not be a closure formed in a `@MainActor` context.

### Checklist for the user (Guest Room)

Guest Room has to be signed in to iCloud (Settings → Users and Accounts). Build, install and launch as in the T5 device checklist, with `--console`.

1. **First launch:** the console shows `frontline: saves: up to date (N saves)`, and `uploaded …` for each save already on the TV.
2. **Saving in the game** (the mission log, or the save after a mission): `uploaded <name>.gci` appears within a few seconds.
3. **Restore from iCloud:**
   - Delete the app from the TV (that clears its Caches).
   - Reinstall and launch: `downloaded <name>.gci`.
   - The save loads in the game.
4. **Deleting a save** in the game's memory card screen: `deleted <name>.gci from iCloud`.
5. **Offline:**
   - Disconnect the TV's network and launch. Play waits at most 5 seconds, and the game starts with the local saves. Save once.
   - Reconnect and relaunch: `uploaded …` for that save.
6. **Optional:** in the CloudKit Console (icloud.developer.apple.com) → the container → Development → Private Database → zone `MemoryCard`, the records are listed. Sign in with the Apple ID the TV uses.
7. **Optional, with a second Apple TV or a later reinstall:** a save changed on both sides keeps the newer copy, and the older one appears under `Memory Card A Conflicts/` in the app's Caches.

**Report back:** the `frontline: saves:` lines, and anything that failed.

**T6 result (2026-11-02, user):** saving works on Guest Room after the fix. The rest of the T6 checklist is still open: restore after a reinstall, delete, offline.

## T7: performance on the A15

**Step 1, measuring (2026-11-02). Testing stop: the user runs the measurement below.** The defaults, MetalFX and shader precompiling are decided from its numbers.

**Added to `FRONTLINE_PERF=1`:**
- **`[perf] memory: footprint … MB (peak … MB), … MB more available`**, once a second.
  - The footprint and peak are the task's `phys_footprint`, which is what jetsam counts.
  - "More available" is `os_proc_available_memory()`, tvOS only: how far the app is from being ended.
- **`[perf] GX shader library compiled in … ms`** and **`[perf] mesh shader library compiled in … ms`:** the run-time MSL compiles, once at startup. They run in the background; the ubershader covers until the specialized pipelines are ready.

**What the results decide:**
- **tvOS Rewritten defaults** (resolution, MSAA, renderer): the best that holds 60 fps, meaning a frame of 16.7 ms or less with GPU time comfortably under it.
- **MetalFX:** if 4K misses 60 on the GPU but 1080p holds it, add a Rewritten "4K (upscaled)" resolution: render at 1080p and upscale with MetalFX spatial in `display.mm`'s present.
- **A precompiled `.metallib` for tvOS:** only if the compiles take several seconds or more.
- **Memory:** check that the peak stays well clear of the limit. On the Mac the footprint is about 170 MB in a level, plus GPU caches.

### Measurement checklist (Guest Room)

Use the optimized build: the Debug build runs the game code unoptimized, so its numbers mean nothing for this.

```bash
cmake --build --preset tvos-release -- -sdk appletvos -allowProvisioningUpdates
xcrun devicectl device install app --device "Guest Room" build/tvos/apps/ios/RelWithDebInfo-appletvos/FrontlineTV.app
```

For each scenario:
1. Pick the settings on the setup screen.
2. Launch with the perf report, saving the console to a file:
   ```bash
   xcrun devicectl device process launch --console --device "Guest Room" --environment-variables '{"FRONTLINE_PERF":"1"}' com.vanillacoffeesoft.FrontlineRewritten 2>&1 | tee perf-<scenario>.txt
   ```
3. Play D-Day to the beach after the landing cutscene, about 30 seconds of play in the same spot each time, then quit the app.

| Scenario | Mode | Resolution | Widescreen | Anti-aliasing | Renderer |
|---|---|---|---|---|---|
| `classic` | Classic | (fixed) | (fixed) | (fixed) | (fixed) |
| `gx-1080` | Rewritten | 1080p | on | Off | GX |
| `gx-1080-msaa` | Rewritten | 1080p | on | 4× MSAA | GX |
| `gx-4k` | Rewritten | 4K | on | Off | GX |
| `gx-4k-msaa` | Rewritten | 4K | on | 4× MSAA | GX |
| `modern-1080-msaa` | Rewritten | 1080p | on | 4× MSAA | Modern |
| `modern-4k-msaa` | Rewritten | 4K | on | 4× MSAA | Modern |

Leave draw distance and the other options at their defaults. Note the TV's resolution (Settings → Video and Audio → Format): a 1080p TV can't show the 4K rows at full size, though they still render at 4K.

**Report back:**
- the `perf-*.txt` files, or at least the `[perf]` lines from the beach;
- how long it took from Play to the title screen in the first run;
- anything that looked or felt wrong (stutter, audio crackle, heat or fan noise).

### Results

Not run. The user closed T7 (2026-11-02): the game runs well on Guest Room as it is.
