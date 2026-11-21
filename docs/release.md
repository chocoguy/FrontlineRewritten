# Releasing

A release is a git tag (`v0.3.0`) and a GitHub release with two files:
- **`FrontlineInstaller-<version>.zip`:** the installer, signed with a Developer ID and notarized.
- **`appcast.xml`:** Sparkle's feed. Installers read it from `releases/latest/download/appcast.xml` to update themselves.

Players' installers see the new tag on GitHub, build the game from it on their Mac, and reinstall it everywhere it's installed.

`tools/release.sh` does all of it on your Mac. Nothing signs or publishes in CI.

## One-time setup

1. **Developer ID certificate:**
   - In Xcode → Settings → Accounts, pick team `USLHQ524FW` and choose Manage Certificates… → + → **Developer ID Application**.
   - Or create it on developer.apple.com → Certificates.
   - Check it's in the keychain: `security find-identity -v -p codesigning` lists `Developer ID Application: … (USLHQ524FW)`.
2. **Notarization credentials**, kept in the keychain under the profile name the script uses:
   ```sh
   xcrun notarytool store-credentials frontline-notary --apple-id <your Apple ID> --team-id USLHQ524FW
   ```
   It asks for an app-specific password: make one at account.apple.com → Sign-In and Security → App-Specific Passwords.
3. **Sparkle's signing key**, once: the private key goes in your keychain, and the public key goes in the repository.
   1. Build the installer once (`cmake --build --preset xcode --target FrontlineInstaller`), which downloads Sparkle into `build/xcode/_deps/sparkle-src`.
   2. Run:
      ```sh
      build/xcode/_deps/sparkle-src/bin/generate_keys
      ```
   3. Put the public key it prints in `apps/installer/sparkle-public-key.txt`, and commit it. Back up the private key (`generate_keys -x <file>`): **without it, installers already out there can't take updates.**
4. **GitHub CLI:**
   ```sh
   brew install gh
   ```
   ```sh
   gh auth login
   ```

## Each release

From a clean `main` that's pushed, with `FrontlineDecompiled`'s commit pushed too:

```sh
tools/release.sh 0.3.0
```

It does this, in order:
1. Checks the above.
2. Opens your editor for the release notes (Markdown), or takes `--notes <file>`.
3. Sets `port.json`'s `version`, commits "Release 0.3.0", and tags `v0.3.0`.
4. Builds Frontline Installer Release, stamped 0.3.0 (build number 300), with Sparkle's public key.
5. Signs Sparkle's parts inside-out, then the app (hardened runtime, timestamp).
6. Notarizes the zip, staples the app, checks it with `spctl`, and zips it again into `build/dist/0.3.0/`.
7. Makes `appcast.xml` with Sparkle's `generate_appcast` (signed with your key, release notes embedded).
8. Asks before pushing, then pushes `main` and the tag, and creates the GitHub release with both files.

If you answer no at the push, the commit and tag stay local. The script prints how to undo them.

**Options:**
- **Release candidates:** `tools/release.sh 0.3.0-rc1 --prerelease`. GitHub marks it a pre-release, so players don't see it (their installers skip pre-releases, and `latest/download` skips them for Sparkle). To test one, turn on pre-releases in your installer:
  ```sh
  defaults write com.vanillacoffeesoft.FrontlineInstaller includePrereleases -bool YES
  ```
- **A release that needs a newer installer:** `--min-app 0.3.0` sets `port.json`'s `minAppVersion`. Older installers then stop before building it and ask to update themselves first.
- **Dry run:** `--dry-run` runs the checks (reporting, not stopping) and builds an ad hoc installer into `build/dist`. It signs, notarizes, commits, tags and pushes nothing.

## Versions
- Tags are `v` plus a semantic version.
- The apps' `CFBundleShortVersionString` is its numeric part, and `CFBundleVersion` is `major×10000 + minor×100 + patch`.
- `FrontlineVersion` (the full version) and `FrontlineCommit` are in every app's Info.plist. `tools/tvos/port installed` and the installer read them back.
