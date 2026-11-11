#!/usr/bin/env python3
"""The SDL bridge must reject guest callbacks and capture before host SDL runs."""

from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parent.parent
runtime = Path(sys.argv[1]).resolve()
output = root / "build/doom-audio-bridge"
output.mkdir(exist_ok=True)
source = output / "audio.c"
guest = output / "audio-x86_64"
source.write_text("""#define SDL_MAIN_HANDLED
#include <SDL.h>
static void callback(void *data, Uint8 *stream, int length) { (void)data; (void)stream; (void)length; }
int main(int argc, char **argv) {
    (void)argv;
    SDL_AudioSpec spec = {0};
    spec.freq = 22050; spec.format = AUDIO_S16LSB; spec.channels = 2;
    if (argc > 1) spec.callback = callback;
    return SDL_OpenAudioDevice(NULL, argc > 1 ? 0 : 1, &spec, NULL, 0);
}
""")
subprocess.run(
    [
        "clang",
        "-target",
        "x86_64-apple-macos11",
        "-O1",
        "-fno-stack-protector",
        "-fno-vectorize",
        "-fno-slp-vectorize",
        "-I/opt/homebrew/opt/sdl2/include/SDL2",
        str(source),
        "-L" + str(root / "build/doom"),
        "-lSDL2",
        "-Wl,-no_fixup_chains",
        "-o",
        str(guest),
    ],
    check=True,
)
for mode in ([], ["callback"]):
    result = subprocess.run(
        [str(runtime), "launch", "--host-libs", str(guest), "--", *mode],
        text=True,
        capture_output=True,
        timeout=15,
    )
    assert result.returncode != 0 and "queued playback only" in result.stderr, (
        result.stdout + result.stderr
    )
    assert "guest callbacks are unsupported" in result.stderr
print(
    "PASS audio bridge: guest callback and capture requests rejected before host audio calls"
)
