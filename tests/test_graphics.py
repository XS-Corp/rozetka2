"""Exercise native GPU/window bridges with an x86 guest, including procedure returns."""

import argparse
import os
from pathlib import Path
import subprocess

p = argparse.ArgumentParser()
p.add_argument("translator", type=Path)
p.add_argument("guest", type=Path)
p.add_argument(
    "--driver", type=Path, default=Path("build/minecraft/host-driver/libMoltenVK.dylib")
)
p.add_argument(
    "--sdl3", type=Path, default=Path("/opt/homebrew/opt/sdl3/lib/libSDL3.dylib")
)
a = p.parse_args()
if not a.driver.is_file() or not a.sdl3.is_file():
    raise SystemExit(
        "Prepare the native graphics driver with scripts/run_minecraft.py --prepare-only and install SDL3 first"
    )
e = dict(
    os.environ,
    R2_HOST_VULKAN=str(a.driver.resolve()),
    R2_HOST_SDL3=str(a.sdl3.resolve()),
)
for mode in ([], ["sdl"], ["callback"]):
    r = subprocess.run(
        [
            str(a.translator.resolve()),
            "launch",
            "--host-libs",
            str(a.guest.resolve()),
            "--",
            *mode,
        ],
        env=e,
        capture_output=True,
        text=True,
        timeout=30,
    )
    if mode == ["callback"]:
        if (
            r.returncode == 0
            or "guest graphics callback is unsupported" not in r.stderr
        ):
            raise SystemExit(r.stdout + r.stderr)
    elif (
        r.returncode != 0
        or "PASS guest graphics ABI:" not in r.stdout
        or "rax = 0 " not in r.stdout
    ):
        raise SystemExit(r.stdout + r.stderr)
print(
    "PASS x86 guest Vulkan procedures, physical GPU, SDL3 surface and callback rejection"
)
