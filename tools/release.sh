#!/bin/sh
# Publishes a release of Frontline Rewritten: tags the version and creates the GitHub release, with
# Frontline Installer attached. Players' installers pick the game up from the tag.
#
# The installer changes only with the major version. N.0.0 builds a new one (signed with your Developer
# ID, notarized and stapled); installers of earlier majors then say a new installer is out, and stop
# updating the game. N.x.y (a game update) attaches vN.0.0's installer again, unchanged, so the latest
# release always has the installer to download.
#
#   tools/release.sh <version> [--prerelease] [--notes <file.md>] [--dry-run]
#
#   <version>        2.0.0, or 2.0.0-rc1 for a release candidate (with --prerelease)
#   --prerelease     a GitHub pre-release: only installers with includePrereleases see it
#   --notes FILE     the release notes (Markdown); asked for otherwise
#   --dry-run        checks everything and builds ad hoc, but signs, notarizes, commits and pushes nothing
#
# One-time setup is in docs/release.md.
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
REPO="chocoguy/FrontlineRewritten"
PROFILE="frontline-notary"
BUILD="$ROOT/build/release-installer"
DIST="$ROOT/build/dist"

version=""
prerelease=0
notes=""
dry=0
while [ $# -gt 0 ]; do
    case "$1" in
        --prerelease) prerelease=1 ;;
        --notes) notes="$2"; shift ;;
        --dry-run) dry=1 ;;
        -*) echo "unknown option $1" >&2; exit 2 ;;
        *) version="$1" ;;
    esac
    shift
done

say() { printf '\n==> %s\n' "$*"; }
fail() { printf 'release: %s\n' "$*" >&2; exit 1; }
# Runs a command, or (dry run) only shows it.
act() {
    if [ "$dry" = 1 ]; then
        printf '   (dry run) %s\n' "$*"
    else
        "$@"
    fi
}

echo "$version" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.]+)?$' || fail "give a version like 0.3.0 or 0.3.0-rc1"
case "$version" in *-*) [ "$prerelease" = 1 ] || fail "$version looks like a release candidate: add --prerelease" ;; esac
tag="v$version"
major="${version%%.*}"
# N.0.0 (or N.0.0-rc1) is a new installer; anything else reuses vN.0.0's.
case "$version" in "$major".0.0|"$major".0.0-*) new_installer=1 ;; *) new_installer=0 ;; esac
installer_tag="v$major.0.0"
ZIP_NAME="FrontlineInstaller.zip"

# A problem that stops a release; a dry run only reports it.
need() {
    if [ "$dry" = 1 ]; then
        echo "   would stop a release: $1"
    else
        fail "$1"
    fi
}

say "Checking"
[ "$(git rev-parse --abbrev-ref HEAD)" = main ] || need "check out main first"
[ -z "$(git status --porcelain)" ] || need "commit or stash your changes first"
git fetch --quiet origin main --tags
[ "$(git rev-parse HEAD)" = "$(git rev-parse origin/main)" ] || need "main differs from origin/main: pull or push first"
! git rev-parse -q --verify "refs/tags/$tag" >/dev/null || fail "$tag already exists here"
[ -z "$(git ls-remote --tags origin "refs/tags/$tag")" ] || fail "$tag already exists on origin"
# Players clone the decomp at the commit main points to: it has to be on GitHub.
sub="$(git -C FrontlineDecompiled rev-parse HEAD)"
git -C FrontlineDecompiled fetch --quiet origin
[ -n "$(git -C FrontlineDecompiled branch -r --contains "$sub" 2>/dev/null)" ] || need "push FrontlineDecompiled's $sub first"
identity="$(security find-identity -v -p codesigning | sed -n 's/.*"\(Developer ID Application: .*\)".*/\1/p' | head -1)"
if [ -z "$identity" ] && [ "$new_installer" = 1 ]; then
    [ "$dry" = 1 ] || fail "no Developer ID Application certificate in the keychain (docs/release.md)"
    echo "   no Developer ID certificate (fine for a dry run)"
fi
team="$(printf '%s' "$identity" | sed -n 's/.*(\([A-Z0-9]*\))$/\1/p')"
if [ "$dry" = 0 ]; then
    [ "$new_installer" = 0 ] || xcrun notarytool history --keychain-profile "$PROFILE" >/dev/null 2>&1 ||
        fail "no notarytool profile $PROFILE (docs/release.md)"
    command -v gh >/dev/null && gh auth status >/dev/null 2>&1 || fail "install gh and run gh auth login (docs/release.md)"
fi
if [ "$new_installer" = 0 ]; then
    if command -v gh >/dev/null && gh release view "$installer_tag" --repo "$REPO" --json assets --jq '.assets[].name' 2>/dev/null |
        grep -qx "$ZIP_NAME"; then
        echo "   game update: attaches $installer_tag's installer"
    else
        need "$installer_tag has no $ZIP_NAME on GitHub: a new major version starts at $major.0.0"
    fi
else
    echo "   new major version: builds Frontline Installer $version"
fi
if [ -z "$notes" ]; then
    notes="$(mktemp -t frontline-notes).md"
    printf '## Frontline Rewritten %s\n\n- \n' "$version" >"$notes"
    ${EDITOR:-vi} "$notes"
fi
[ -s "$notes" ] || fail "the release notes are empty"
if [ "$new_installer" = 1 ]; then
    echo "   $tag, the installer signed by ${identity:-nobody (dry run)}"
else
    echo "   $tag"
fi

say "Bumping port.json"
mkdir -p "$(dirname "$BUILD")"
[ "$dry" = 0 ] || cp port.json "$BUILD.port.json"
python3 -I - "$version" "$major.0.0" <<'EOF'
import json, sys
from pathlib import Path
path = Path('port.json')
text = path.read_text()
data = json.loads(text)
version, min_app = sys.argv[1], sys.argv[2]
# Keep the file's layout: replace the two values in place. minAppVersion (the major's installer) is
# only for the 1.x installers, which compared it before major versions meant installers: they stop
# before building a later major.
old_version = json.dumps(data['version'])
text = text.replace(f'"version": {old_version}', f'"version": {json.dumps(version)}', 1)
if 'minAppVersion' in data:
    text = text.replace(f'"minAppVersion": {json.dumps(data["minAppVersion"])}', f'"minAppVersion": {json.dumps(min_app)}', 1)
json.loads(text)
path.write_text(text)
EOF
act git commit --quiet -am "Release $version"
act git tag -a "$tag" -m "Frontline Rewritten $version"
if [ "$dry" = 1 ]; then
    grep -E '"(version|minAppVersion)"' port.json | sed 's/^/   would be: /'
    mv "$BUILD.port.json" port.json
fi
commit="$(git rev-parse --short HEAD)"

rm -rf "$DIST/$version"
mkdir -p "$DIST/$version"
zip="$DIST/$version/$ZIP_NAME"
if [ "$new_installer" = 1 ]; then
    say "Building Frontline Installer $version"
    sign_identity="${identity:--}"
    [ "$dry" = 0 ] || sign_identity="-"
    cmake -S "$ROOT" -B "$BUILD" -G Xcode \
        -DFRONTLINE_VERSION="$version" -DFRONTLINE_COMMIT="$commit" -DFRONTLINE_INSTALLER_VERSION="$version" \
        -DFRONTLINE_INSTALLER_SIGN_IDENTITY="$sign_identity" -DFRONTLINE_DEVELOPMENT_TEAM="$team" >/dev/null
    app="$BUILD/apps/installer/Release/Frontline Installer.app"
    # A fresh bundle: Xcode leaves files an earlier build put there (Sparkle.framework, before 2.0.0).
    rm -rf "$app"
    cmake --build "$BUILD" --config Release --target FrontlineInstaller -- -quiet
    [ -d "$app" ] || fail "the build made no app"

    say "Signing"
    act codesign --force --options runtime --timestamp --sign "$identity" "$app"
    act codesign --verify --deep --strict --verbose=2 "$app"

    say "Notarizing"
    ditto -c -k --keepParent "$app" "$zip"
    act xcrun notarytool submit "$zip" --keychain-profile "$PROFILE" --wait
    act xcrun stapler staple "$app"
    act spctl --assess --type execute --verbose=2 "$app"
    rm -f "$zip"
    ditto -c -k --keepParent "$app" "$zip"
else
    say "Fetching $installer_tag's installer"
    act gh release download "$installer_tag" --repo "$REPO" --pattern "$ZIP_NAME" --dir "$DIST/$version"
fi

say "Publishing"
if [ "$dry" = 1 ]; then
    echo "   (dry run) git push origin main $tag"
    echo "   (dry run) gh release create $tag $zip"
    if [ "$new_installer" = 1 ]; then
        echo "Dry run done: $zip (ad hoc signed)."
    else
        echo "Dry run done."
    fi
    exit 0
fi
printf 'Push %s and publish the release on GitHub? [y/N] ' "$tag"
read -r answer
case "$answer" in
    y|Y|yes) ;;
    *) echo "Stopped before pushing. The commit and $tag are local: git tag -d $tag && git reset --hard HEAD~1 undoes them."; exit 1 ;;
esac
git push origin main "$tag"
pre=""
[ "$prerelease" = 0 ] || pre="--prerelease"
# shellcheck disable=SC2086
gh release create "$tag" "$zip" --repo "$REPO" --verify-tag \
    --title "Frontline Rewritten $version" --notes-file "$notes" $pre
echo "Published $tag: https://github.com/$REPO/releases/tag/$tag"
