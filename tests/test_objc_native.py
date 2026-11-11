from pathlib import Path
import os
import subprocess
import sys

root = Path(__file__).resolve().parent.parent
binary = Path(sys.argv[1]).resolve()
guest = root / "build/objc-guest-x86_64"
subprocess.run(
    [
        "clang",
        "-target",
        "x86_64-apple-macos11",
        "-O1",
        "-fno-stack-protector",
        "-Wl,-no_fixup_chains",
        str(root / "tests/objc_guest.m"),
        "-o",
        str(guest),
        "-lobjc",
        "-framework",
        "Foundation",
    ],
    check=True,
)
for mode in ([], ["block"]):
    result = subprocess.run(
        [str(binary), "launch", "--host-libs", str(guest), "--", *mode],
        env=dict(os.environ, R2_JVM_PROBE="1"),
        capture_output=True,
        text=True,
        timeout=20,
    )
    if mode:
        if (
            result.returncode == 0
            or "unsupported Objective-C argument ABI" not in result.stderr
        ):
            raise SystemExit(result.stdout + result.stderr)
    elif (
        result.returncode
        or "PASS Objective-C native encodings" not in result.stdout
        or "rax = 0 " not in result.stdout
    ):
        raise SystemExit(result.stdout + result.stderr)
print(
    "PASS ObjC scope, native signatures, FP ABI, guest x86 methods, callback identity and block rejection"
)
