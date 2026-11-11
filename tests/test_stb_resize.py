"""Compare official x86 STB resizing with same-version native scalar output."""

from pathlib import Path
import hashlib
import math
import struct
import subprocess
import sys
import urllib.request

root = Path(__file__).resolve().parent.parent
binary = Path(sys.argv[1]).resolve()
work = root / "build/minecraft/stb-test"
work.mkdir(parents=True, exist_ok=True)
header = work / "stb_image_resize2.h"
url = "https://raw.githubusercontent.com/LWJGL/lwjgl3/3.4.3/modules/lwjgl/stb/src/main/c/stb_image_resize2.h"
# Pin the exact vendored header matching the installed JNI library's release.
expected = "5de9c29faed66feb70d7f5a6e94a95ea30c6581a808c045fe56b84df4c949940"
if not header.is_file() or hashlib.sha256(header.read_bytes()).hexdigest() != expected:
    header.write_bytes(urllib.request.urlopen(url, timeout=30).read())
if hashlib.sha256(header.read_bytes()).hexdigest() != expected:
    raise SystemExit("Unexpected STB reference source hash")
source = root / "tests/stb_resize_guest.c"
guest = work / "resize-x86_64"
reference = work / "resize-reference-arm64"
subprocess.run(
    [
        "clang",
        "-arch",
        "arm64",
        "-O2",
        "-DSTB_IMAGE_RESIZE_IMPLEMENTATION",
        "-DSTBIR_NO_SIMD",
        "-DR2_STB_REFERENCE",
        "-I" + str(work),
        str(source),
        "-o",
        str(reference),
    ],
    check=True,
)
subprocess.run(
    [
        "clang",
        "-target",
        "x86_64-apple-macos11",
        "-O1",
        "-fno-stack-protector",
        "-Wl,-no_fixup_chains",
        "-I" + str(work),
        str(source),
        "-o",
        str(guest),
    ],
    check=True,
)
expected_file = work / "resize-reference.bin"
actual_file = work / "resize-actual.bin"
subprocess.run([str(reference), str(expected_file)], check=True, capture_output=True)
with (work / "resize.log").open("w") as log:
    result = subprocess.run(
        [
            str(binary),
            "launch",
            "--host-libs",
            str(guest),
            "--",
            str(root / "build/minecraft/natives/lwjgl/3.4.3+4/x64/liblwjgl_stb.dylib"),
            str(actual_file),
        ],
        stdout=log,
        stderr=subprocess.STDOUT,
        timeout=180,
    )
output = (work / "resize.log").read_text()
if (
    result.returncode
    or "PASS STB resizing 150 cases" not in output
    or "rax = 0 " not in output
):
    raise SystemExit(output)
a = actual_file.read_bytes()
b = expected_file.read_bytes()
position = 0
cases = 0
max_byte_error = 0
max_float_error = 0
while position < len(b):
    kind, size = struct.unpack_from("<II", b, position)
    if a[position : position + 8] != b[position : position + 8]:
        raise SystemExit("Resize fixture metadata mismatch")
    position += 8
    actual = a[position : position + size]
    expected_pixels = b[position : position + size]
    if len(actual) != size:
        raise SystemExit("Truncated resized output")
    if kind < 2:
        error = max(abs(x - y) for x, y in zip(actual, expected_pixels))
        max_byte_error = max(max_byte_error, error)
        if error > 1:
            raise SystemExit(f"Resize case {cases}: byte error {error}")
    else:
        av = struct.unpack("<" + "f" * (size // 4), actual)
        bv = struct.unpack("<" + "f" * (size // 4), expected_pixels)
        for x, y in zip(av, bv):
            error = abs(x - y)
            max_float_error = max(max_float_error, error)
            if not math.isfinite(x) or error > 1e-4 * max(1, abs(y)):
                raise SystemExit(f"Resize case {cases}: float {x} != {y}")
    cases += 1
    position += size
if len(a) != len(b) or cases != 150:
    raise SystemExit("Wrong resize case count")
print(
    f"PASS actual x86 STB resize vs ARM scalar: {cases} cases; maximum byte error {max_byte_error}, float error {max_float_error:.3g}"
)
