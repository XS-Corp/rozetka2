#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
VERSION=9.3
SHA256=a9affc52c9596ee0682bcdd4ceb3c0b235eb221d83d733dfd9b0dff45897cb0d
WORK="$ROOT/build/nano"
ARCHIVE="$WORK/nano-$VERSION.tar.xz"
SOURCE="$WORK/nano-$VERSION"
mkdir -p "$WORK"
# Apple's SDK exports wide curses from libncurses itself, without a separate
# libncursesw name. This local linker alias does not change the system SDK.
SDK=$(xcrun --show-sdk-path)
ln -sf "$SDK/usr/lib/libncurses.5.4.tbd" "$WORK/libncursesw.tbd"
if [ ! -f "$ARCHIVE" ]; then
    curl -fL "https://www.nano-editor.org/dist/v9/nano-$VERSION.tar.xz" -o "$ARCHIVE"
fi
printf '%s  %s\n' "$SHA256" "$ARCHIVE" | shasum -a 256 -c -
if [ ! -d "$SOURCE" ]; then
    tar -xf "$ARCHIVE" -C "$WORK"
fi
cd "$SOURCE"
# The host/build distinction prevents configure from executing any x86 binary
# through the operating system (and therefore from invoking Apple's Rosetta).
./configure --build=aarch64-apple-darwin --host=x86_64-apple-darwin \
    --enable-tiny --enable-utf8 --disable-nls --disable-libmagic \
    --disable-dependency-tracking \
    CC='clang -target x86_64-apple-macos11' \
    CFLAGS='-O1 -g -fno-stack-protector -fno-vectorize -fno-slp-vectorize -fno-asynchronous-unwind-tables' \
    LDFLAGS="-Wl,-no_fixup_chains -L$WORK" > "$WORK/configure.log" 2>&1
# Documentation does not need rebuilding; makeinfo is not a runtime dependency.
make -C lib clean > "$WORK/clean-lib.log" 2>&1
make -C src clean > "$WORK/clean-src.log" 2>&1
make -C lib -j4 > "$WORK/build-lib.log" 2>&1
make -C src -j4 > "$WORK/build-src.log" 2>&1
file src/nano
printf '\nRun:\n  %s/build/rozetka2 launch --host-libs %s/src/nano -- FILE\n' "$ROOT" "$SOURCE"
