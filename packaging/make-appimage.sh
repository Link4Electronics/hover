#!/bin/sh

# Build the Linux release as an AppImage, in one script, run from the Arch
# container of the appimage job in .github/workflows/ci.yml (tag pushes and
# manual dispatch only), the only Linux build of the workflow. Same shape as
# dethrace's and carpocalypse2's packaging/make-appimage.sh: install the
# distro SDL2, build, stage a flat AppDir, then let quick-sharun bundle the
# libraries and appimagetool turn it into an image. The tooling (quick-sharun,
# appimagetool) comes from pkgforge-dev/anylinux-setup-action@v2 in the
# workflow, not from this repo.
#
# No game data is bundled, and none may be: the repository ships no original
# game code or assets (see README.md, Legal). The AppRun hook stages a
# per-user game directory instead and tells the player what to copy there.
#
# src/recomp/gen is recompiler output and is not in the repository either (see
# REPO_RULES section 3). When it is absent the configure below falls back to
# CMake's empty dispatch so the image still builds; the game only runs once
# the lifted C is present.

set -eu

ARCH=$(uname -m)
export ARCH
export OUTPATH=./dist
export APPNAME=hover
export STARTUPWMCLASS=hover
export DEPLOY_OPENGL=1
export ADD_HOOKS="self-updater.hook"
# gh-releases-zsync|owner|repo|channel|pattern. The channel field is a
# reserved keyword, not a tag name: "latest" resolves to the newest
# non-prerelease release, which is exactly what a version tag push creates via
# the release job. Guarded so the script can also be run outside Actions,
# where there is no repository to update from and UPINFO would only make the
# tooling emit a .zsync pointing at nothing.
if [ -n "${GITHUB_REPOSITORY:-}" ]; then
    export UPINFO="gh-releases-zsync|${GITHUB_REPOSITORY%/*}|${GITHUB_REPOSITORY#*/}|latest|*${ARCH}.AppImage.zsync"
fi

echo "Installing package dependencies..."
echo "---------------------------------------------------------------"
pacman -Syu --noconfirm \
    cmake     \
    libdecor  \
    patchelf  \
    sdl2      \
    sdl2_ttf

echo "Installing debloated packages..."
echo "---------------------------------------------------------------"
# The display/audio stack the bundled libSDL2 dlopens, in smaller variants.
# SDL2 itself comes from Arch's sdl2 package above, which pulls its build
# dependencies; pkg_check_modules(SDL2) then resolves through
# /usr/lib/pkgconfig/sdl2.pc.
get-debloated-pkgs --add-common --prefer-nano

echo "Building hover..."
echo "---------------------------------------------------------------"
# Version from the checked-out tree. A short commit hash keeps VERSION.txt
# simple; the workflow names the release after the tag it was pushed with,
# not after this.
VERSION="$(git rev-parse --short HEAD)"
export VERSION

# src/recomp/gen is generated locally from the player's copy (see README,
# "What is not in this repo"). Without it CMake falls back to the empty
# dispatch stub and the image enters 0x00000000 instead of the game. CI has
# no game files, so it sets HOVER_ALLOW_STUB=1 to still exercise packaging.
if ! ls src/recomp/gen/*.c >/dev/null 2>&1; then
    if [ "${HOVER_ALLOW_STUB:-0}" = 1 ]; then
        echo "WARNING: src/recomp/gen is empty; packaging a stub image" >&2
    else
        echo "error: src/recomp/gen is empty; this image would not contain the game." >&2
        echo "  Run 'python3 run_lift.py' (it needs game/hover/HOVER.EXE) and" >&2
        echo "  then this script again, or set HOVER_ALLOW_STUB=1 for a stub." >&2
        exit 1
    fi
fi

mkdir -p ./dist
echo "${VERSION}" > ./dist/VERSION.txt

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    -DHOVER_GEN_STUB=ON \
    -DPCRECOMP="${PCRECOMP:-$PWD/pcrecomp}"
cmake --build build -j"$(nproc)"

mkdir -p ./AppDir/bin
mv -v ./build/hover ./AppDir/bin/

# Desktop entry + icon. The icon must be the entry's Icon= value with no
# extension, next to the .desktop file; quick-sharun surfaces that as the
# AppImage icon.
cp -v ./packaging/hover.desktop ./AppDir/hover.desktop
cp -v ./packaging/icon_source.png ./AppDir/hover.png

# First-run hook: creates the per-user game directory, explains what to copy
# into it, and cd's there so the game's cwd-relative file lookups land in
# writable storage rather than inside the read-only mount.
cp -v ./packaging/setup-data.hook ./AppDir/bin/

# MIT keeps the licence with the binary it covers.
mkdir -p ./AppDir/usr/share/licenses/hover
cp -v ./LICENSE ./AppDir/usr/share/licenses/hover/LICENSE

# The game parses decimals out of text files with strtod-family calls, which
# read the decimal separator from the ambient locale -- a comma where a dot
# was expected silently corrupts the parsed value. The original ran under the
# C locale; pin it for the same reason dethrace and carpocalypse2 do.
echo 'LC_ALL=C.UTF-8' >> ./AppDir/.env

# Deploy the shared libraries the binary needs (libSDL2, libSDL2_ttf and their
# companions) into the AppDir, then turn it into an AppImage (+ .zsync for
# UPINFO above).
quick-sharun ./AppDir/bin/hover
quick-sharun --make-appimage
