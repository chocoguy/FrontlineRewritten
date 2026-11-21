# Frontline Installer status

Hand-maintained. The standalone installer: a signed, notarized download that builds the game at a GitHub release tag and keeps every install up to date. It replaces the in-repository `Frontline Installer.app`, which was removed 2026-11-21.

**Built (2026-11-21). Testing stop: the user runs the checklist below.**
- Compiled, and its logic is tested (`frontline_installertest`).
- `tools/release.sh --dry-run` passed.
- Nothing has been signed with a Developer ID, notarized or published, and the installer's window hasn't been opened.

**The user's choices (2026-11-21):**
- Updates come only from GitHub release tags.
- Both repositories are public.
- The installer updates itself with Sparkle.
- Releases are made by `tools/release.sh` on the user's Mac.

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

**R3: installs and reminders.**
- `state.json` records each install's version, commit, install date, expiry and whether it's pending.
- At launch, `port installed` checks what's really there.
- `Reminders` schedules local notifications 14 and 3 days before a device's profile expires.

**R4: releases.**
- Sparkle 2.10.1 comes from `FetchContent`, pinned by GitHub's SHA-256, and is embedded and code-signed on copy.
- The feed is `releases/latest/download/appcast.xml`. Builds without `FRONTLINE_SPARKLE_PUBLIC_KEY` never offer to update themselves.
- `tools/release.sh` and [docs/release.md](../release.md).

**R5: cleanup.**
- **Docs:** the README's Install section is now the download; how to build the installer is under Building.
- **Removed:** the repository-root `Frontline Installer.app`. On first launch, the new installer imports the old one's disc choice and deletes its compiled copy (`~/Library/Application Support/Frontline Installer`).
- **`port.json`:** `minAppVersion` is now `0.1.0`. It was `1.0`, which no 0.x installer would have satisfied.

## Open items
- **The first release has to come from a source that has all this.** An installer can only build tags whose `tools/tvos/port` takes `--platform` and `--work-dir` and reports `installed`, which is every tag from the first release on.
- **Sparkle's update UI** follows the user's choice in its first-run prompt; automatic checks are on by default.
- **Reminders** can be checked only with a real device install, unless the expiry is temporarily shortened in a debug build.

## Checklist for the user

**1. One-time setup** ([docs/release.md](../release.md)):
- [ ] the Developer ID certificate
- [ ] the `frontline-notary` profile
- [ ] Sparkle keys: commit `apps/installer/sparkle-public-key.txt`, and back up the private key
- [ ] `gh`

**2. Release candidate:** commit this work, then:

```sh
tools/release.sh 0.1.0-rc1 --prerelease
```

- [ ] It finishes and the GitHub release has the zip and `appcast.xml`.
- [ ] On another macOS user account (System Settings → Users & Groups), download the zip from the release page, unzip it and open it: no Gatekeeper warning.
- [ ] In that account, pre-releases are on:
  ```sh
  defaults write com.vanillacoffeesoft.FrontlineInstaller includePrereleases -bool YES
  ```

**3. Fresh setup** (that account):
- [ ] Setup lists what's missing. Download gets CMake; Install gets iOS support.
- [ ] Choose the disc, then Install On… → This Mac. The source downloads, it builds, and Frontline Rewritten opens from Applications.
- [ ] With the paid team signed in to Xcode: an iPhone, and Guest Room, each install and launch. The Game screen shows each one's version and when it stops working.

**4. Game update:**

```sh
tools/release.sh 0.1.1-rc1 --prerelease
```

- [ ] Updates shows 0.1.1-rc1 with its notes. Update All builds once per platform and reinstalls every target.
- [ ] With the Apple TV turned off, it's marked pending. Finish the Update completes it later.

**5. Installer update:**

```sh
tools/release.sh 0.1.2-rc1 --prerelease --min-app 0.1.2
```

- [ ] Sparkle offers the new installer and relaunches it. (Sparkle reads `latest/download`, which skips pre-releases, so testing this step needs a full release, or the appcast URL pointed at the candidate's.)
- [ ] An older installer told to update to 0.1.2 stops with "needs Frontline Installer 0.1.2 or later".

**6. Rollback:**
- [ ] Undo Mac Update puts the previous Mac version back.
- [ ] Install Another Version → 0.1.0-rc1 reinstalls the older one everywhere.

**7. Reminders:**
- [ ] After a device install, System Settings → Notifications lists Frontline Installer. A debug build with a short expiry shows the reminder.
