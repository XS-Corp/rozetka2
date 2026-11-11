"""Verify grouped macOS ABI conversions without a Minecraft startup."""

import os
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parent.parent
binary = Path(sys.argv[1]).resolve()
guest = root / "build/platform-guest-x86_64"
subprocess.run(
    [
        "clang",
        "-target",
        "x86_64-apple-macos11",
        "-O1",
        "-fno-stack-protector",
        "-Wl,-no_fixup_chains",
        str(root / "tests/platform_guest.c"),
        "-framework",
        "CoreFoundation",
        "-framework",
        "CoreText",
        "-framework",
        "CoreGraphics",
        "-framework",
        "CFNetwork",
        "-o",
        str(guest),
    ],
    check=True,
)
for mode in ([], ["array"], ["dictionary"], ["mutable-dictionary"]):
    result = subprocess.run(
        [str(binary), "launch", "--host-libs", str(guest), "--", *mode],
        env=dict(os.environ, R2_JVM_PROBE="1"),
        capture_output=True,
        text=True,
        timeout=30,
    )
    if not mode:
        if (
            result.returncode
            or "PASS platform batch:" not in result.stdout
            or "rax = 0 " not in result.stdout
        ):
            raise SystemExit(result.stdout + result.stderr)
    elif (
        result.returncode == 0
        or (
            "guest CFArray callbacks are unsupported"
            if mode == ["array"]
            else "guest CFDictionary callbacks are unsupported"
        )
        not in result.stderr
    ):
        raise SystemExit(result.stdout + result.stderr)
print("PASS macOS platform batch and guest callback-table guards")
