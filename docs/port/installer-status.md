# Frontline Installer status

Hand-maintained. The standalone installer: a signed, notarized download that builds the game at a GitHub release tag and keeps every install up to date. It replaces the in-repository `Frontline Installer.app`, which was removed 2026-11-21.

**Built (2026-11-21). Testing stop: the user runs the checklist below.**
- Compiled, and its logic is tested (`frontline_installertest`).
- `tools/release.sh --dry-run` passed.
- Nothing has been signed with a Developer ID, notarized or published, and the installer's window hasn't been opened.

**The user's choices (2026-11-21):**
- Updates come only from GitHub release tags.
- Both repositories are public.
- ~~The installer updates itself with Sparkle.~~ Replaced 2026-11-27: the major version is the installer (below).
- Releases are made by `tools/release.sh` on the user's Mac.

## 2026-11-27: major version = installer (built, untested)

Ported from GT3 Revisited's installer (the user's migration guide).
- **No Sparkle.** `N.0.0` releases build, sign and notarize a new installer; `N.x.y` releases re-attach `vN.0.0`'s `FrontlineInstaller.zip` unchanged. An installer builds only its own major (`Releases.compatible`); a newer major shows an alert and a banner, and blocks game updates (Reinstall still works). `--min-app` is gone; `release.sh` sets `minAppVersion` to `N.0.0` only to stop the 1.x Sparkle installers.
- **Transition:** the first release with this installer must be `2.0.0`. 1.x installers' Check for Updates… finds nothing; players download it by hand.
- **Fixed:** Add/Reinstall built `state.tag` even after a failed first build pinned an old tag (`targetTag`); Updates with nothing installed now says to install from the Game screen.
- **`tools/tvos/port build`:** passes `-DCMAKE_OSX_SYSROOT` (an `xcrun` shim exports the macOS `SDKROOT`), builds with `xcodebuild -scheme … -derivedDataPath <project>/DerivedData`, and removes the `CMAKE_TRY_COMPILE-*` folders configure leaves in the global DerivedData.
- **Setup → Storage:** size, Show in Finder, Free Up Space (`work/xcode`, `xcode-ios`, `xcode-mac`), Remove Everything….
- **Accent #8AA6EE** in the installer and the Mac game app; the Mac game app has the installer's icon (`apps/macos/AppIcon.icns`).

## What was built

**R0: version stamping.**
- The root `CMakeLists.txt` takes `FRONTLINE_VERSION` and `FRONTLINE_COMMIT`.
  - `PROJECT_VERSION` is the version's numeric part.
  - `FRONTLINE_BUILD_NUMBER` (`major×10000 + minor×100 + patch`) is every app's `CFBundleVersion`.
  - `FrontlineVersion` and `FrontlineCommit` are in the Mac and iOS/tvOS Info.plists.
- `tools/tvos/port` reads them from git (the last tag, and HEAD) and passes them when it configures. Its `result` events carry `version` and `commit`.
- The new `port installed` reads back what's installed: the Mac's Applications, or `devicectl device info apps` for a device.

**R1: the app** (`apps/installer`, target `FrontlineInstaller`, Xcode generator only).

| Part | What it does |
|---|---|
| `Workspace` / `InstallerState` | `~/Library/Application Support/Frontline Rewritten Installer`: `source/`, `work/` (port's `--work-dir`), `tools/`, `logs/`, `state.json` |
| `Source` | git: a partial clone, `fetch --tags`, checking out the tag and its submodule. It reads `port.json`'s `minAppVersion` at the tag before checking out, and goes back to the previous tag if a checkout fails. |
| `Releases` | GitHub's releases API, with an ETag cache. Pre-releases only with the `includePrereleases` default. |
| `SemVer` | version comparison |
| `Shell` / `LineReader` | runs tools. Lines are delivered in order on the main queue before a run returns; Cancel stops the whole process tree. |
| `AppModel` | the jobs (below) |
| `Views` | the Setup, Game, Updates and Activity screens |

`AppModel`'s jobs:
- **Install on:** a new target, or a reinstall.
- **Update All:** builds once per platform, then installs per target. A device that can't be reached is marked pending, and Finish the Update retries it.
- **Install Another Version:** any release, for going back.
- **Undo Mac Update:** puts back the copy kept in `work/previous`.
- **Downloads:** CMake, and Xcode's iOS and tvOS platforms.

**R2: Setup checks** (`Doctor`):
- **Xcode:** selected, version 27, its first launch done.
- **Python:** 3.9 or later.
- **CMake:** 3.31 or later, either Kitware's 4.5.1 that the installer downloads (checked against its SHA-256) or one already on the Mac.
- **Optional:** the iOS and tvOS platforms (installed with `xcodebuild -downloadPlatform`), and a paid team in Xcode (read from Xcode's defaults, as the port script does).

**R3: installs.**
- `state.json` records each install's version, commit, install date, expiry and whether it's pending.
- At launch, `port installed` checks what's really there.
- The Game screen shows each device's expiry date. There are no notifications (removed 2026-11-21 at the user's request).
- The installer installs only; it never opens the app it installed (also 2026-11-21). An install used to end with `--launch`, and devicectl's launch could leave the job hanging at "Starting it".
- **Fixed (2026-11-21):** the build warned that a device "is not in the app's profile" for every device. Profiles list hardware UDIDs, and `--device` is devicectl's identifier; the script now looks up the device's UDID (`hardware_udid`) before comparing.

**R4: releases.**
- (Sparkle was removed 2026-11-27, above.)
- `tools/release.sh` and [docs/release.md](../release.md).

**R5: cleanup.**
- **Docs:** the README's Install section is now the download; how to build the installer is under Building.
- **Removed:** the repository-root `Frontline Installer.app`. On first launch, the new installer imports the old one's disc choice and deletes its compiled copy (`~/Library/Application Support/Frontline Installer`).
- **`port.json`:** `minAppVersion` is now `0.1.0`. It was `1.0`, which no 0.x installer would have satisfied.

## Open items
- **The first release has to come from a source that has all this.** An installer can only build tags whose `tools/tvos/port` takes `--platform` and `--work-dir` and reports `installed`, which is every tag from the first release on.

## Checklist for the user

**1. One-time setup** ([docs/release.md](../release.md)):
- [ ] the Developer ID certificate
- [ ] the `frontline-notary` profile
- [ ] `gh`

**2. Release candidate:** commit this work, then:

```sh
tools/release.sh 2.0.0-rc1 --prerelease
```

- [ ] It finishes and the GitHub release has `FrontlineInstaller.zip`.
- [ ] On another macOS user account (System Settings → Users & Groups), download the zip from the release page, unzip it and open it: no Gatekeeper warning.
- [ ] In that account, pre-releases are on:
  ```sh
  defaults write com.vanillacoffeesoft.FrontlineInstaller includePrereleases -bool YES
  ```

**3. Fresh setup** (that account):
- [ ] Setup lists what's missing. Download gets CMake; Install gets iOS support.
- [ ] Choose the disc, then Install On… → This Mac. The source downloads, it builds, and Frontline Rewritten opens from Applications.
- [ ] With the paid team signed in to Xcode: an iPhone, and Guest Room, each install (without opening). The Game screen shows each one's version and when it stops working.

**4. Game update:**

```sh
tools/release.sh 2.0.1-rc1 --prerelease
```

- [ ] The release has 2.0.0-rc1's installer zip, re-attached.
- [ ] Updates shows 2.0.1-rc1 with its notes. Update All builds once per platform and reinstalls every target.
- [ ] With the Apple TV turned off, it's marked pending. Finish the Update completes it later.

**5. New installer:**

```sh
tools/release.sh 3.0.0-rc1 --prerelease
```

- [ ] The 2.x installer shows "A new Frontline Installer is available" once, and a banner on Game and Updates. Update All and Install Another Version… are gone or disabled; Reinstall still works.
- [ ] Setup → Storage shows the folder's size; Free Up Space shrinks it.

**6. Rollback:**
- [ ] Undo Mac Update puts the previous Mac version back.
- [ ] Install Another Version → 2.0.0-rc1 reinstalls the older one everywhere.

