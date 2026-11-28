# Releasing

A release is a git tag (`v2.0.0`) and a GitHub release with one file, **`FrontlineInstaller.zip`**: the installer, signed with a Developer ID and notarized.

Players' installers see the new tag on GitHub, build the game from it on their Mac, and reinstall it everywhere it's installed.

**The major version is the installer's.** The installer doesn't update itself:
- **`N.0.0` is a new installer.** The script builds, signs and notarizes it. Installers of an earlier major version show an alert that a new installer is available, link to the release, and stop updating the game until the player uses the new one. Reinstall keeps working, so free Apple IDs can renew before the 7 days run out.
- **`N.x.y` is a game update.** No new installer: the script attaches `vN.0.0`'s zip again, unchanged, so the latest release always has the installer to download. Installers of major `N` update the game to it.

An installer builds only the releases of its own major version. Everything it makes is in `~/Library/Application Support/Frontline Rewritten Installer`, not in the app, so replacing the app with a new download changes nothing else.

`tools/tvos/port` ships with the game, not the installer (the installer runs it from the tag it builds), so fixes to it reach players in an `N.x.y` release.

`tools/release.sh` does all of it on your Mac. Nothing signs or publishes in CI.

**Moving from the Sparkle installers (1.x):** the first release with the new installer is `2.0.0`. Release `2.0.0` sets `port.json`'s `minAppVersion` to `2.0.0`, so 1.x installers stop before building it and ask to update, but their Check for Updates… finds nothing (there's no appcast any more). Players download the 2.0.0 installer by hand: say so in the release notes.

## One-time setup

1. **Developer ID certificate** (only for a new major version):
   - In Xcode → Settings → Accounts, pick team `USLHQ524FW` and choose Manage Certificates… → + → **Developer ID Application**.
   - Or create it on developer.apple.com → Certificates.
   - Check it's in the keychain: `security find-identity -v -p codesigning` lists `Developer ID Application: … (USLHQ524FW)`.
2. **Notarization credentials** (only for a new major version), kept in the keychain under the profile name the script uses:
   ```sh
   xcrun notarytool store-credentials frontline-notary --apple-id <your Apple ID> --team-id USLHQ524FW
   ```
   It asks for an app-specific password: make one at account.apple.com → Sign-In and Security → App-Specific Passwords.
3. **GitHub CLI:**
   ```sh
   brew install gh
   ```
   ```sh
   gh auth login
   ```

## Each release

From a clean `main` that's pushed, with `FrontlineDecompiled`'s commit pushed too:

```sh
tools/release.sh 2.0.0
```

It does this, in order:
1. Checks the above, and that the tag is new here and on GitHub. For a game update (`2.1.0`), checks `v2.0.0` on GitHub has the installer.
2. Opens your editor for the release notes (Markdown), or takes `--notes <file>`.
3. Sets `port.json`'s `version` (and `minAppVersion` to `N.0.0`, for the 1.x installers that still read it), commits "Release 2.0.0", and tags `v2.0.0`.
4. **New major version only:** builds Frontline Installer (Release), stamped 2.0.0 (build number 20000), signs it (hardened runtime, timestamp), notarizes the zip, staples the app, checks it with `spctl`, and zips it again into `build/dist/2.0.0/`.
   **Game update:** downloads `vN.0.0`'s `FrontlineInstaller.zip` into `build/dist/<version>/` instead.
5. Asks before pushing, then pushes `main` and the tag, and creates the GitHub release with the zip.

If you answer no at the push, the commit and tag stay local. The script prints how to undo them.

**Options:**
- **Release candidates:** `tools/release.sh 2.0.0-rc1 --prerelease`. GitHub marks it a pre-release, so players don't see it (their installers skip pre-releases). To test one, turn on pre-releases in your installer:
  ```sh
  defaults write com.vanillacoffeesoft.FrontlineInstaller includePrereleases -bool YES
  ```
- **A release that needs a new installer:** give it the next major version (`3.0.0`).
- **Dry run:** `--dry-run` runs the checks (reporting, not stopping) and, for a new major version, builds an ad hoc installer into `build/dist`. It signs, notarizes, commits, tags and pushes nothing.

## Versions
- Tags are `v` plus a semantic version.
- The installer's version is the `N.0.0` release it was built for (`FrontlineInstallerVersion` in its Info.plist; local builds take `port.json`'s); the game's is the release's.
- The apps' `CFBundleShortVersionString` is its numeric part, and `CFBundleVersion` is `major×10000 + minor×100 + patch`.
- `FrontlineVersion` (the full version) and `FrontlineCommit` are in every game app's Info.plist. `tools/tvos/port installed` and the installer read them back.
