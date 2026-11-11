from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parent.parent
binary = Path(sys.argv[1]).resolve()
guest = root / "build/thread-signals-x86_64"
subprocess.run(
    [
        "clang",
        "-target",
        "x86_64-apple-macos11",
        "-O1",
        "-fno-stack-protector",
        "-Wl,-no_fixup_chains",
        str(root / "tests/signals_threads_guest.c"),
        "-o",
        str(guest),
    ],
    check=True,
)
result = subprocess.run(
    [str(binary), "launch", "--host-libs", str(guest)],
    capture_output=True,
    text=True,
    timeout=30,
)
if (
    result.returncode
    or "PASS virtual pthread signals" not in result.stdout
    or "rax = 0 " not in result.stdout
):
    raise SystemExit(result.stdout + result.stderr)
print(
    "PASS x86 thread-directed signals, inherited/temporary masks, pending delivery and signal longjmp"
)
