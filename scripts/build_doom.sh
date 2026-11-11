#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
WORK="$ROOT/build/doom"
COMMIT=e257a09527062bfb986b6e5e0780839b6cbd5b71
SOURCE="$WORK/doomgeneric-$COMMIT"
SDL=/opt/homebrew/opt/sdl2
if [ ! -f "$SDL/lib/libSDL2.dylib" ]; then
    echo "SDL2 is required at $SDL (brew install sdl2)." >&2
    exit 1
fi
mkdir -p "$WORK"
if [ ! -f "$WORK/doomgeneric.tar.gz" ]; then
    curl -fL "https://github.com/ozkl/doomgeneric/archive/$COMMIT.tar.gz" -o "$WORK/doomgeneric.tar.gz"
fi
if [ ! -f "$WORK/freedoom-0.13.0.zip" ]; then
    curl -fL https://github.com/freedoom/freedoom/releases/download/v0.13.0/freedoom-0.13.0.zip -o "$WORK/freedoom-0.13.0.zip"
fi
printf '%s  %s\n' 26f8f60daa6c6e4d3c01e5eac9fb700d592dede803d13ac848154ff356b2a2e7 "$WORK/doomgeneric.tar.gz" | shasum -a 256 -c -
printf '%s  %s\n' 3f9b264f3e3ce503b4fb7f6bdcb1f419d93c7b546f4df3e874dd878db9688f59 "$WORK/freedoom-0.13.0.zip" | shasum -a 256 -c -
if [ ! -d "$SOURCE" ]; then tar -xf "$WORK/doomgeneric.tar.gz" -C "$WORK"; fi
if [ ! -d "$WORK/freedoom-0.13.0" ]; then unzip -q "$WORK/freedoom-0.13.0.zip" -d "$WORK"; fi

# Only linker metadata is x86_64. Runtime SDL is the installed arm64 library;
# the original engine and our platform adapter are compiled as guest code.
cat > "$WORK/libSDL2.tbd" <<'EOF'
--- !tapi-tbd-v3
archs: [ x86_64 ]
platform: macosx
install-name: /opt/homebrew/opt/sdl2/lib/libSDL2.dylib
current-version: 2.0.0
compatibility-version: 2.0.0
exports:
  - archs: [ x86_64 ]
    symbols: [ _SDL_SetMainReady, _SDL_Init, _SDL_GetError, _SDL_CreateWindow, _SDL_CreateRenderer, _SDL_CreateTexture, _SDL_UpdateTexture, _SDL_RenderClear, _SDL_RenderCopy, _SDL_RenderPresent, _SDL_PollEvent, _SDL_PushEvent, _SDL_Delay, _SDL_GetTicks, _SDL_SetWindowTitle, _SDL_DestroyTexture, _SDL_DestroyRenderer, _SDL_DestroyWindow, _SDL_Quit, _SDL_InitSubSystem, _SDL_GetCurrentAudioDriver, _SDL_OpenAudioDevice, _SDL_CloseAudioDevice, _SDL_PauseAudioDevice, _SDL_QueueAudio, _SDL_GetQueuedAudioSize, _SDL_ClearQueuedAudio, _SDL_GetAudioDeviceStatus ]
...
EOF
python3 - "$ROOT" "$SOURCE/doomgeneric" "$WORK" "$SDL" <<'PY'
import pathlib, re, sys
root, source, work, sdl = map(pathlib.Path, sys.argv[1:])
original = (source / 'Makefile').read_text()
objects = re.search(r'^SRC_DOOM = (.+)$', original, re.M).group(1).split()
objects.remove('doomgeneric_xlib.o')
objects.append('doom_sdl.o')
objects.extend(['doom_audio.o', 'mus2mid.o'])
# Upstream's dispatcher includes SDL_mixer.h but uses no mixer declarations.
# Keep the engine untouched; compile this generated dispatcher copy instead.
(work / 'i_sound.c').write_text((source / 'i_sound.c').read_text().replace('#include <SDL_mixer.h>', '/* SDL_mixer is not used by this platform backend. */'))
(work / 'doom.mk').write_text(f'''SOURCE := {source}
ROOT := {root}
OBJS := {' '.join(str(work / 'objects' / obj) for obj in objects)}
CC := clang -target x86_64-apple-macos11
CFLAGS := -std=gnu11 -O2 -g -MMD -MP -fno-stack-protector -fno-vectorize -fno-slp-vectorize -ffp-contract=off -Wno-int-conversion -Wno-incompatible-pointer-types -Wno-format -DNORMALUNIX -DLINUX -D_DEFAULT_SOURCE -DFEATURE_SOUND -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200 -I$(SOURCE) -I$(ROOT)/ports -I{sdl}/include/SDL2
all: {work}/doom-x86_64
{work}/doom-x86_64: $(OBJS)
\t$(CC) $(OBJS) -o $@ -Wl,-no_fixup_chains -L{work} -lSDL2 -framework AudioToolbox -framework CoreFoundation
{work}/objects/doom_sdl.o: $(ROOT)/ports/doom_sdl.c $(ROOT)/scripts/build_doom.sh
\t@mkdir -p $(@D)
\t$(CC) $(CFLAGS) -c $< -o $@
{work}/objects/doom_audio.o: $(ROOT)/ports/doom_audio.c $(ROOT)/scripts/build_doom.sh
\t@mkdir -p $(@D)
\t$(CC) $(CFLAGS) -c $< -o $@
{work}/objects/i_sound.o: {work}/i_sound.c $(ROOT)/scripts/build_doom.sh
\t@mkdir -p $(@D)
\t$(CC) $(CFLAGS) -c $< -o $@
{work}/objects/%.o: $(SOURCE)/%.c $(ROOT)/scripts/build_doom.sh
\t@mkdir -p $(@D)
\t$(CC) $(CFLAGS) -c $< -o $@
-include $(OBJS:.o=.d)
''')
PY
if ! make -f "$WORK/doom.mk" -j4 > "$WORK/build.log" 2>&1; then
    tail -60 "$WORK/build.log" >&2
    exit 1
fi
file "$WORK/doom-x86_64"
printf '\nBuilt DoomGeneric with Freedoom. Launch with: make run-doom\n'
