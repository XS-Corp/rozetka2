"""Run real native sorting with x86 comparators, including nested reentry."""

from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parent.parent
binary = Path(sys.argv[1]).resolve()
work = root / "build/sort-test"
work.mkdir(parents=True, exist_ok=True)
guest = work / "sort-x86_64"
subprocess.run(
    [
        "clang",
        "-target",
        "x86_64-apple-macos11",
        "-O1",
        "-fno-stack-protector",
        "-Wl,-no_fixup_chains",
        str(root / "tests/sort_guest.c"),
        "-o",
        str(guest),
    ],
    check=True,
)
for mode, message in [
    ([], None),
    (["fail"], "unsupported 0f opcode 0x0b"),
    (["overflow"], "array size overflows"),
    (["memory"], "not writable"),
    (["target"], "outside guest executable code"),
]:
    result = subprocess.run(
        [str(binary), "launch", "--host-libs", str(guest), "--", *mode],
        capture_output=True,
        text=True,
        timeout=40,
    )
    if message:
        if result.returncode == 0 or message not in result.stderr:
            raise SystemExit(result.stdout + result.stderr)
    elif (
        result.returncode
        or "PASS x86 qsort/qsort_r" not in result.stdout
        or "rax = 0 " not in result.stdout
    ):
        raise SystemExit(result.stdout + result.stderr)
print(
    "PASS native qsort/qsort_r with x86 callbacks, nested sorts, lifetime stress, bounds and failure propagation"
)
