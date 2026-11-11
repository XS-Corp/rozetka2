#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUNDLE="$ROOT/build/Quake-rozetka2.app/Contents"
mkdir -p "$BUNDLE/MacOS" "$BUNDLE/Resources/id1"
clang -target arm64-apple-macos11 -Wall -Wextra -Werror -O2 "$ROOT/ports/quake_launcher.c" -o "$BUNDLE/MacOS/Quake"
cp "$ROOT/build/rozetka2" "$BUNDLE/MacOS/rozetka2"
cp "$ROOT/build/quake/quake-x86_64" "$BUNDLE/Resources/quake-x86_64"
cp "$ROOT/build/quake/data/id1/pak0.pak" "$BUNDLE/Resources/id1/pak0.pak"
cp "$ROOT/build/quake/shareware/SLICNSE.TXT" "$BUNDLE/Resources/Quake-Shareware-License.txt"
cp "$ROOT/build/quake/shareware/LICINFO.TXT" "$BUNDLE/Resources/Quake-License-Info.txt"
cp "$ROOT/build/quake/tyrquake-653157915975b196e36980a1ef7146485509b69a/gnu.txt" "$BUNDLE/Resources/TyrQuake-GPL.txt"
cat > "$BUNDLE/Resources/BUILD.txt" <<'EOF'
Local development build for rozetka2. TyrQuake's engine and the SDL adapters
execute as x86_64 guest code; only declared SDL/libc APIs use native libraries.
TyrQuake GPL source: https://github.com/sezero/tyrquake
Commit: 653157915975b196e36980a1ef7146485509b69a
Source archive: ../../../quake/tyrquake.tar.gz (beside this local bundle).
Original shareware archive: ../../../quake/quake106.zip
SDL2: /opt/homebrew/opt/sdl2/lib/libSDL2.dylib
This local bundle includes shareware episode data for testing, with original
license notices. It is not a standalone redistribution package. Preserve the
original archives and applicable notices when sharing builds.
Rebuild with: make quake-app
EOF
cat > "$BUNDLE/Info.plist" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleIdentifier</key><string>org.rozetka2.quake</string>
<key>CFBundleName</key><string>Quake rozetka2</string>
<key>CFBundleDisplayName</key><string>Quake rozetka2</string>
<key>CFBundleExecutable</key><string>Quake</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleVersion</key><string>1</string>
<key>NSHighResolutionCapable</key><true/>
</dict></plist>
EOF
printf 'Built %s/build/Quake-rozetka2.app\n' "$ROOT"
