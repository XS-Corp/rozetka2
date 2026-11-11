#!/usr/bin/env python3
"""Play the real guest engine through SDL events and verify rendered/audio output."""

import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import time
import wave

sys.dont_write_bytecode = True
from test_doom import png

runtime, game, data = map(lambda p: Path(p).resolve(), sys.argv[1:4])
output = Path("build/quake-test").resolve()
output.mkdir(parents=True, exist_ok=True)
prefix = output / "frame"
for old in output.glob("frame-*"):
    old.unlink()
env = os.environ.copy()
env.pop("R2_QUAKE_FRAMES", None)
env.update(
    R2_QUAKE_TEST_KEYS="1",
    R2_QUAKE_CAPTURE=str(prefix),
    R2_QUAKE_AUDIO_CAPTURE=str(output / "effects.wav"),
    R2_QUAKE_USERDIR=str(output / "userdata"),
)
command = [
    str(runtime),
    "launch",
    "--host-libs",
    str(game),
    "--",
    "-basedir",
    str(data),
    "-noconinput",
    "-noudp",
    "-noextmusic",
    "+viewsize",
    "90",
    "+map",
    "e1m1",
]
started = time.monotonic()
with (output / "run.log").open("w") as log:
    result = subprocess.run(
        command, cwd=output, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=180
    )
text = (output / "run.log").read_text()
assert result.returncode == 0 and "rax = 0 " in text, text[-6000:]
fields = ("frame", "time", "x", "y", "yaw", "ammo")
states = {
    match[0]: dict(zip(fields, map(float, match[1:])))
    for match in re.findall(
        r"ROZETKA2_QUAKE state (\w+) frame=(\d+) time=([\d.-]+) x=([\d.-]+) y=([\d.-]+) yaw=([\d.-]+) ammo=(\d+)",
        text,
    )
}
assert all(
    label in states for label in ("before", "moved", "turned", "fired", "mouse")
), text[-6000:]
before, moved, turned, fired = (
    states[label] for label in ("before", "moved", "turned", "fired")
)
assert (before["x"], before["y"]) != (
    moved["x"],
    moved["y"],
), "forward SDL input did not move player"
assert moved["yaw"] != turned["yaw"], "right SDL input did not turn player"
assert fired["ammo"] < turned["ammo"], "fire SDL input did not consume shells"
assert (
    states["mouse"]["yaw"] != fired["yaw"]
), "SDL relative mouse motion did not turn the view"
models = re.findall(
    r"viewmodel name=progs/v_shot.mdl models=(\d+) viewport_height=(\d+)", text
)
assert models and all(
    int(count) > 0 and 0 < int(height) <= 152 for count, height in models
), "weapon was hidden beneath HUD"
frames = {}
for label, state in states.items():
    raw = Path(f"{prefix}-{label}.argb").read_bytes()
    width, height, number = struct.unpack("<III", raw[:12])
    assert (width, height, number) == (320, 200, state["frame"])
    pixels = raw[12:]
    assert len(pixels) == width * height * 4
    assert len(set(struct.unpack(f"<{width * height}I", pixels))) > 64, "empty frame"
    frames[label] = pixels
    png(Path(f"{prefix}-{label}.png"), width, height, pixels)
assert (
    frames["before"] != frames["moved"] != frames["turned"]
), "game view did not change"
stats = re.search(
    r"audio stats nonzero=(\d+) queued=(\d+) consumed=(\d+) playing=(\d+)", text
)
assert stats, text[-6000:]
nonzero, queued, consumed, playing = map(int, stats.groups())
assert nonzero > 100 and queued >= consumed > 0 and playing == 1, stats.group()
with wave.open(str(output / "effects.wav"), "rb") as audio:
    assert (audio.getnchannels(), audio.getsampwidth(), audio.getframerate()) == (
        2,
        2,
        22050,
    )
    pcm = audio.readframes(audio.getnframes())
assert len(pcm) == queued
values = struct.unpack(f"<{len(pcm) // 2}h", pcm)
assert (
    sum(value != 0 for value in values) == nonzero
    and max(values) > 100
    and min(values) < -100
)
print(
    f"PASS Quake x86_64: e1m1, SDL movement/turn/fire/mouse, visible weapon, audio queue, clean exit ({time.monotonic() - started:.1f}s)"
)
print(f"Frames, audio, and log: {output}")
