#!/usr/bin/env python3
"""Real guest engine, real SDL event path, deterministic gameplay assertions."""

import argparse
import os
from pathlib import Path
import re
import struct
import subprocess
import zlib
import wave


def png(path, width, height, pixels):
    def chunk(kind, data):
        return (
            struct.pack(">I", len(data))
            + kind
            + data
            + struct.pack(">I", zlib.crc32(kind + data))
        )

    rgb = b"".join(
        bytes((pixels[i + 2], pixels[i + 1], pixels[i]))
        for i in range(0, len(pixels), 4)
    )
    rows = b"".join(
        b"\0" + rgb[y * width * 3 : (y + 1) * width * 3] for y in range(height)
    )
    path.write_bytes(
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(rows))
        + chunk(b"IEND", b"")
    )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("runtime", type=Path)
    parser.add_argument("game", type=Path)
    parser.add_argument("wad", type=Path)
    parser.add_argument(
        "--sound-mode",
        choices=("normal", "nosound", "nomusic", "nosfx"),
        default="normal",
    )
    parser.add_argument(
        "--mus-fixture",
        action="store_true",
        help="exercise original MUS conversion with a short looping score",
    )
    args = parser.parse_args()
    runtime, game, wad = (p.resolve() for p in (args.runtime, args.game, args.wad))
    suffix = ("-mus" if args.mus_fixture else "") + (
        "" if args.sound_mode == "normal" else "-" + args.sound_mode
    )
    output = Path("build/doom-test" + suffix).resolve()
    output.mkdir(parents=True, exist_ok=True)
    prefix = output / "frame"
    env = os.environ.copy()
    env.pop("R2_DOOM_FRAMES", None)
    audio_path = output / "effects.wav"
    audio_path.unlink(missing_ok=True)
    env.update(
        R2_DOOM_TEST_KEYS="1",
        R2_DOOM_CAPTURE=str(prefix),
        R2_DOOM_AUDIO_CAPTURE=str(audio_path),
    )
    for old in output.glob("frame-*"):
        old.unlink()
    command = [
        str(runtime),
        "launch",
        "--host-libs",
        str(game),
        "--",
        "-iwad",
        str(wad),
        "-config",
        str(output / "doom.cfg"),
        "-warp",
        "1",
        "-skill",
        "1",
        "-nomonsters",
    ]
    if args.sound_mode != "normal":
        command.append("-" + args.sound_mode)
    if args.mus_fixture:
        # One piano note, released after 70 MUS ticks, then 70 ticks of rest.
        # This exercises conversion and repeated playback without proprietary data.
        score = bytes((0x40, 0, 0, 0x90, 60 | 128, 100, 70, 0x80, 60, 70, 0x60))
        mus = struct.pack("<4s6H", b"MUS\x1a", len(score), 16, 1, 0, 0, 0) + score
        fixture = output / "music.wad"
        fixture.write_bytes(
            struct.pack("<4sII", b"PWAD", 1, 12 + len(mus))
            + mus
            + struct.pack("<II8s", 12, len(mus), b"D_E1M1")
        )
        command.extend(("-file", str(fixture)))
    with (output / "run.log").open("w") as log:
        result = subprocess.run(
            command,
            cwd=output,
            env=env,
            stdout=log,
            stderr=subprocess.STDOUT,
            timeout=90,
        )
    text = (output / "run.log").read_text()
    assert result.returncode == 0 and "rax = 0 " in text, text[-4000:]
    fields = ("frame", "tic", "x", "y", "angle", "ammo")
    states = {
        match[0]: dict(zip(fields, map(int, match[1:])))
        for match in re.findall(
            r"ROZETKA2_DOOM state (\w+) frame=(\d+) tic=(\d+) x=(-?\d+) y=(-?\d+) angle=(\d+) ammo=(\d+)",
            text,
        )
    }
    assert all(
        label in states for label in ("before", "moved", "turned", "fired")
    ), text[-4000:]
    before, moved, turned, fired = (
        states[label] for label in ("before", "moved", "turned", "fired")
    )
    assert (before["x"], before["y"]) != (
        moved["x"],
        moved["y"],
    ), "SDL forward input did not move player"
    assert moved["angle"] != turned["angle"], "SDL right input did not turn player"
    assert fired["ammo"] < turned["ammo"], "SDL fire input did not consume ammo"
    frames = {}
    for label, state in states.items():
        raw = Path(f"{prefix}-{label}.argb").read_bytes()
        width, height, frame = struct.unpack("<III", raw[:12])
        assert (width, height, frame) == (320, 200, state["frame"])
        pixels = raw[12:]
        assert len(pixels) == width * height * 4
        assert (
            len(set(struct.unpack(f"<{width * height}I", pixels))) > 64
        ), "empty or flat frame"
        frames[label] = pixels
        png(Path(f"{prefix}-{label}.png"), width, height, pixels)
    assert (
        frames["before"] != frames["moved"] != frames["turned"]
    ), "game view did not change"
    if args.sound_mode not in ("nosound", "nosfx"):
        stats = re.search(
            r"audio stats starts=(\d+) nonzero=(\d+) queued=(\d+) consumed=(\d+) playing=(\d+)",
            text,
        )
        assert stats, text[-4000:]
        starts, nonzero, queued, consumed, playing = map(int, stats.groups())
        assert (
            starts > 0 and nonzero > 100 and queued > consumed > 0 and playing == 1
        ), stats.group()
        assert (
            "audio start name=pistol " in text
        ), "fire did not start the WAD pistol sound"
        with wave.open(str(audio_path), "rb") as sound:
            assert (
                sound.getnchannels(),
                sound.getsampwidth(),
                sound.getframerate(),
            ) == (2, 2, 22050)
            pcm = sound.readframes(sound.getnframes())
        assert len(pcm) == queued
        values = struct.unpack(f"<{len(pcm) // 2}h", pcm)
        assert (
            max(values) > 1000 and min(values) < -1000
        ), "silent or invalid audio waveform"
        assert sum(v != 0 for v in values) == nonzero, "capture differs from queued PCM"
    else:
        assert (
            "audio initialized" not in text and not audio_path.exists()
        ), "disabled SFX opened a device"
    if args.sound_mode not in ("nosound", "nomusic"):
        assert re.search(
            r"music started tracks=[1-9]\d* looping=1", text
        ), "MIDI track did not load"
        assert (
            "music running advancing=1" in text
        ), "music clock or native synthesis graph did not start"
        assert (
            "music registered format=" + ("MUS" if args.mus_fixture else "MIDI") in text
        )
    else:
        assert "music started" not in text, "disabled music still started"
    print(
        f"PASS Doom x86_64 ({args.sound_mode}): gameplay, audio output, music, clean exit"
    )
    print(f"Frames and log: {output}")


if __name__ == "__main__":
    main()
