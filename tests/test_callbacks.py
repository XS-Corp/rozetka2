"""Exercise real SDL reentry and a native-thread callback backend with x86 targets."""

from pathlib import Path
import os
import subprocess
import sys

root = Path(__file__).resolve().parent.parent
binary = Path(sys.argv[1]).resolve()
work = root / "build/minecraft/callback-test"
work.mkdir(parents=True, exist_ok=True)
guest = work / "callbacks-x86_64"
backend = work / "native-thread.dylib"
subprocess.run(
    [
        "clang",
        "-target",
        "x86_64-apple-macos11",
        "-O1",
        "-fno-stack-protector",
        "-Wl,-no_fixup_chains",
        str(root / "tests/callbacks_guest.c"),
        "-o",
        str(guest),
    ],
    check=True,
)
subprocess.run(
    [
        "clang",
        "-arch",
        "arm64",
        "-dynamiclib",
        "-O1",
        str(root / "tests/callbacks_native.c"),
        "-Wl,-reexport_library,/opt/homebrew/opt/sdl3/lib/libSDL3.dylib",
        "-o",
        str(backend),
    ],
    check=True,
)
for mode, path in [
    ([], Path("/opt/homebrew/opt/sdl3/lib/libSDL3.dylib")),
    (["foreign"], backend),
    (["persistent"], backend),
    (["fail"], backend),
]:
    environment = dict(os.environ, R2_HOST_SDL3=str(path))
    result = subprocess.run(
        [str(binary), "launch", "--host-libs", str(guest), "--", *mode],
        env=environment,
        capture_output=True,
        text=True,
        timeout=30,
    )
    if mode == ["fail"]:
        if result.returncode == 0 or "unsupported 0f opcode 0x0b" not in result.stderr:
            raise SystemExit(result.stdout + result.stderr)
    elif (
        result.returncode
        or "PASS guest SDL callbacks:" not in result.stdout
        or "rax = 0 " not in result.stdout
    ):
        raise SystemExit(result.stdout + result.stderr)
print(
    "PASS x86 callback reentry, original stack, foreign threads, nested calls and failure shutdown"
)
