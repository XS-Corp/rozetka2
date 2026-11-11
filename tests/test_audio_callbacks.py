from pathlib import Path
import os
import subprocess
import sys

root = Path(__file__).resolve().parent.parent
binary = Path(sys.argv[1]).resolve()
guest = root / "build/audio-callbacks-x86_64"
subprocess.run(
    [
        "clang",
        "-target",
        "x86_64-apple-macos11",
        "-O1",
        "-fno-stack-protector",
        "-Wl,-no_fixup_chains",
        str(root / "tests/audio_callbacks_guest.c"),
        "-framework",
        "AudioToolbox",
        "-framework",
        "CoreAudio",
        "-o",
        str(guest),
    ],
    check=True,
)
for mode in ([], ["invalid"], ["retire"]):
    result = subprocess.run(
        [str(binary), "launch", "--host-libs", str(guest), "--", *mode],
        env=dict(os.environ, R2_JVM_PROBE="1"),
        capture_output=True,
        text=True,
        timeout=30,
    )
    if mode == ["invalid"]:
        if (
            result.returncode == 0
            or "callback target is outside guest executable code" not in result.stderr
        ):
            raise SystemExit(result.stdout + result.stderr)
    elif (
        result.returncode
        or "PASS audio callback" not in result.stdout
        or "rax = 0 " not in result.stdout
    ):
        raise SystemExit(result.stdout + result.stderr)
print(
    "PASS CoreAudio callbacks, original guest pointers, PCM buffers, return values and retirement"
)
