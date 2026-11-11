#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
WORK="$ROOT/build/quake"
COMMIT=653157915975b196e36980a1ef7146485509b69a
SOURCE="$WORK/tyrquake-$COMMIT"
SDL=/opt/homebrew/opt/sdl2
if [ ! -f "$SDL/lib/libSDL2.dylib" ]; then
    echo "SDL2 is required at $SDL (brew install sdl2)." >&2
    exit 1
fi
mkdir -p "$WORK"
if [ ! -f "$WORK/tyrquake.tar.gz" ]; then
    curl -fL "https://github.com/sezero/tyrquake/archive/$COMMIT.tar.gz" -o "$WORK/tyrquake.tar.gz"
fi
if [ ! -f "$WORK/quake106.zip" ]; then
    curl -fL https://ftp.gwdg.de/pub/misc/ftp.idsoftware.com/idstuff/quake/quake106.zip -o "$WORK/quake106.zip"
fi
printf '%s  %s\n' d4c2fdd7f186487d2da8174376ae917ebf1fecd210577fc2bcd956501b76339c "$WORK/tyrquake.tar.gz" | shasum -a 256 -c -
printf '%s  %s\n' ec6c9d34b1ae0252ac0066045b6611a7919c2a0d78a3a66d9387a8f597553239 "$WORK/quake106.zip" | shasum -a 256 -c -
if [ ! -d "$SOURCE" ]; then tar -xf "$WORK/tyrquake.tar.gz" -C "$WORK"; fi
mkdir -p "$WORK/shareware" "$WORK/data/id1"
if [ ! -f "$WORK/shareware/resource.1" ]; then unzip -oq "$WORK/quake106.zip" -d "$WORK/shareware"; fi
# macOS libarchive reads the original LHa self-extracting archive as data;
# neither the DOS installer nor any x86 executable is run by the OS.
bsdtar -xf "$WORK/shareware/resource.1" -C "$WORK/shareware" ID1/PAK0.PAK SLICNSE.TXT LICINFO.TXT README.TXT
cp "$WORK/shareware/ID1/PAK0.PAK" "$WORK/data/id1/pak0.pak"
printf '%s  %s\n' 35a9c55e5e5a284a159ad2a62e0e8def23d829561fe2f54eb402dbc0a9a946af "$WORK/data/id1/pak0.pak" | shasum -a 256 -c -
cat > "$WORK/libSDL2.tbd" <<'EOF'
--- !tapi-tbd-v3
archs: [ x86_64 ]
platform: macosx
install-name: /opt/homebrew/opt/sdl2/lib/libSDL2.dylib
current-version: 2.0.0
compatibility-version: 2.0.0
exports:
  - archs: [ x86_64 ]
    symbols: [ _SDL_SetMainReady, _SDL_Init, _SDL_GetError, _SDL_CreateWindow, _SDL_CreateRenderer, _SDL_CreateTexture, _SDL_UpdateTexture, _SDL_RenderClear, _SDL_RenderCopy, _SDL_RenderPresent, _SDL_PollEvent, _SDL_PushEvent, _SDL_Quit, _SDL_DestroyTexture, _SDL_DestroyRenderer, _SDL_DestroyWindow, _SDL_InitSubSystem, _SDL_GetCurrentAudioDriver, _SDL_OpenAudioDevice, _SDL_CloseAudioDevice, _SDL_PauseAudioDevice, _SDL_QueueAudio, _SDL_GetQueuedAudioSize, _SDL_ClearQueuedAudio, _SDL_GetAudioDeviceStatus, _SDL_SetRelativeMouseMode ]
...
EOF
python3 - "$ROOT" "$SOURCE" "$WORK" <<'PY'
from pathlib import Path
import re, sys
root, source, work = map(Path, sys.argv[1:])
text = (source / 'common/in_null.c').read_text().replace('cvar_t _windowed_mouse;', 'cvar_t _windowed_mouse = {"_windowed_mouse", "0"};')
(work / 'in_port.c').write_text(text)
# Keep configuration under the task's directory without repurposing HOME.
text = (source / 'common/sys_unix.c').read_text().replace('getenv("HOME")', 'getenv("R2_QUAKE_USERDIR")').replace('"%s/.tyrquake", home', '"%s", home')
text = text.replace('    return NULL;\n}', '    return "./userdata";\n}', 1)
(work / 'sys_port.c').write_text(text)
makefile = (source / 'Makefile').read_text()
objects = []
for group in ('COMMON_OBJS', 'CL_OBJS', 'SV_OBJS', 'NQCL_OBJS', 'SW_OBJS'):
    block = re.search(r'^' + group + r' := \\\n(.*?)(?:\n\n)', makefile, re.M | re.S).group(1)
    objects += re.findall(r'([\w]+)\.o', block)
objects += ['net_udp', 'net_bsd', 'cd_null']
files = [next(p for p in (source / 'common' / f'{obj}.c', source / 'NQ' / f'{obj}.c') if p.exists()) for obj in objects]
files += [root / 'ports/quake_sdl.c', root / 'ports/quake_audio.c', work / 'in_port.c', work / 'sys_port.c']
text = f'''CC := clang -target x86_64-apple-macos11
CFLAGS := -std=gnu99 -mlong-double-64 -O2 -g -MMD -MP -fno-stack-protector -fno-vectorize -fno-slp-vectorize -ffp-contract=off -fno-strict-aliasing -Wno-int-conversion -Wno-incompatible-pointer-types -Wno-format -DTYR_VERSION=0.71 -DTYR_VERSION_TIME=1793390400LL -DQBASEDIR=. -DNQ_HACK -DNDEBUG -I{source}/include -I{source}/NQ -I{source}/external -I/opt/homebrew/opt/sdl2/include/SDL2
OBJS := {' '.join(str(work / 'objects' / (p.stem + '.o')) for p in files)}
all: {work}/quake-x86_64
{work}/quake-x86_64: $(OBJS)
\t$(CC) $(OBJS) -Wl,-no_fixup_chains -L{work} -lSDL2 -o $@
'''
for p in files:
    text += f'{work}/objects/{p.stem}.o: {p} {root}/scripts/build_quake.sh\n\t@mkdir -p $(@D)\n\t$(CC) $(CFLAGS) -c $< -o $@\n'
text += '-include $(OBJS:.o=.d)\n'
(work / 'quake.mk').write_text(text)
PY
if ! make -f "$WORK/quake.mk" -j4 > "$WORK/build.log" 2>&1; then
    tail -50 "$WORK/build.log" >&2
    exit 1
fi
file "$WORK/quake-x86_64"
printf '\nBuilt TyrQuake x86_64 with shareware data. Launch with: make run-quake\n'
