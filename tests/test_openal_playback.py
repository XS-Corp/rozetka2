"""Play actual PCM using the official x86 driver and native CoreAudio callbacks."""

import os
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parent.parent
binary = Path(sys.argv[1]).resolve()
guest = root / "build/openal-playback-x86_64"
driver = root / "build/minecraft/natives/lwjgl/3.4.3+4/x64/libopenal.dylib"
if not driver.is_file():
    raise SystemExit("Prepare Minecraft native libraries first")
subprocess.run(
    [
        "clang",
        "-target",
        "x86_64-apple-macos11",
        "-O1",
        "-fno-stack-protector",
        "-Wl,-no_fixup_chains",
        str(root / "tests/openal_playback_guest.c"),
        "-o",
        str(guest),
    ],
    check=True,
)
loopback = "--loopback" in sys.argv[2:]
result = subprocess.run(
    [
        str(binary),
        "launch",
        "--host-libs",
        str(guest),
        "--",
        str(driver),
        *(["--loopback"] if loopback else []),
    ],
    env=dict(
        os.environ,
        R2_JVM_PROBE="1",
        R2_GUEST_CXX_ABI=str(root / "build/minecraft/libcxx/librozetka2-cxxabi.dylib"),
    ),
    capture_output=True,
    text=True,
    timeout=30,
)
if (
    result.returncode
    or "PASS real x86 OpenAL PCM playback" not in result.stdout
    or "rax = 0 " not in result.stdout
):
    raise SystemExit(result.stdout + result.stderr)
if loopback:
    lines = [
        line for line in result.stdout.splitlines() if line.startswith("PASS loopback")
    ]
    if len(lines) != 1:
        raise SystemExit(result.stdout + result.stderr)
    print(lines[0])
else:
    print(
        "PASS official x86 OpenAL: real PCM consumed by CoreAudio, floating point offset and clean shutdown"
    )
