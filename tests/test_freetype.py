from pathlib import Path
import os
import subprocess
import sys

root = Path(__file__).resolve().parent.parent
binary = Path(sys.argv[1]).resolve()
guest = root / "build/freetype-x86_64"
subprocess.run(
    [
        "clang",
        "-target",
        "x86_64-apple-macos11",
        "-O1",
        "-fno-stack-protector",
        "-Wl,-no_fixup_chains",
        "-I/opt/homebrew/opt/freetype/include/freetype2",
        str(root / "tests/freetype_guest.c"),
        "-o",
        str(guest),
    ],
    check=True,
)
result = subprocess.run(
    [
        str(binary),
        "launch",
        "--host-libs",
        str(guest),
        "--",
        str(root / "build/minecraft/natives/lwjgl/3.4.3+4/x64/libfreetype.dylib"),
        "/System/Library/Fonts/Supplemental/Arial.ttf",
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
    or "PASS real x86 FreeType:" not in result.stdout
    or "rax = 0 " not in result.stdout
):
    raise SystemExit(result.stdout + result.stderr)
print(
    "PASS official x86 FreeType: Latin/Cyrillic/Greek rasterization, metrics, kerning and shutdown"
)
