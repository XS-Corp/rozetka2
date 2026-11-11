#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUNDLE="$ROOT/build/Doom-rozetka2.app/Contents"
mkdir -p "$BUNDLE/MacOS" "$BUNDLE/Resources"
clang -target arm64-apple-macos11 -Wall -Wextra -Werror -O2 "$ROOT/ports/doom_launcher.c" -o "$BUNDLE/MacOS/Doom"
cp "$ROOT/build/rozetka2" "$BUNDLE/MacOS/rozetka2"
cp "$ROOT/build/doom/doom-x86_64" "$BUNDLE/Resources/doom-x86_64"
cp "$ROOT/build/doom/freedoom-0.13.0/freedoom1.wad" "$BUNDLE/Resources/freedoom1.wad"
cp "$ROOT/build/doom/freedoom-0.13.0/COPYING.txt" "$BUNDLE/Resources/Freedoom-COPYING.txt"
cp "$ROOT/build/doom/freedoom-0.13.0/CREDITS.txt" "$BUNDLE/Resources/Freedoom-CREDITS.txt"
cp "$ROOT/build/doom/doomgeneric-e257a09527062bfb986b6e5e0780839b6cbd5b71/LICENSE" "$BUNDLE/Resources/DoomGeneric-LICENSE.txt"
cat > "$BUNDLE/Resources/BUILD.txt" <<'EOF'
Local development build. The arm64 launcher executes rozetka2; DoomGeneric
and ports/doom_sdl.c/doom_audio.c execute as x86_64 guest code in its interpreter.
SDL2 is loaded from /opt/homebrew/opt/sdl2/lib/libSDL2.dylib.
Effects are mixed by guest code and queued to SDL. MIDI/MUS music uses the
system AudioToolbox synthesizer through declared C APIs.
DoomGeneric source: https://github.com/ozkl/doomgeneric
Commit: e257a09527062bfb986b6e5e0780839b6cbd5b71
Source archive: ../../../doom/doomgeneric.tar.gz (beside this local bundle).
Freedoom 0.13.0: https://github.com/freedoom/freedoom/releases/tag/v0.13.0
Rebuild: make doom-app in the rozetka2 repository. This is not a standalone
redistribution package; retain source and license files when sharing builds.
EOF
cat > "$BUNDLE/Info.plist" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleIdentifier</key><string>org.rozetka2.doom</string>
<key>CFBundleName</key><string>Doom rozetka2</string>
<key>CFBundleDisplayName</key><string>Doom rozetka2</string>
<key>CFBundleExecutable</key><string>Doom</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleVersion</key><string>1</string>
<key>NSHighResolutionCapable</key><true/>
</dict></plist>
EOF
printf 'Built %s/build/Doom-rozetka2.app\n' "$ROOT"
