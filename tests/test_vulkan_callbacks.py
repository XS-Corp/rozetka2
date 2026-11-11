from pathlib import Path
import os
import subprocess
import sys

root = Path(__file__).resolve().parent.parent
binary = Path(sys.argv[1]).resolve()
guest = root / "build/vulkan-callbacks-x86_64"
subprocess.run(
    [
        "clang",
        "-target",
        "x86_64-apple-macos11",
        "-O1",
        "-fno-stack-protector",
        "-Wl,-no_fixup_chains",
        str(root / "tests/vulkan_callbacks_guest.c"),
        "-o",
        str(guest),
    ],
    check=True,
)
for mode in ([], ["invalid"], ["retire"]):
    result = subprocess.run(
        [str(binary), "launch", "--host-libs", str(guest), "--", *mode],
        env=dict(
            os.environ,
            R2_HOST_VULKAN=str(root / "build/minecraft/host-driver/libMoltenVK.dylib"),
        ),
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
        or "PASS Vulkan callbacks" not in result.stdout
        and "PASS Vulkan debug callbacks" not in result.stdout
        or "rax = 0 " not in result.stdout
    ):
        raise SystemExit(result.stdout + result.stderr)
print(
    "PASS real MoltenVK debug-utils/report guest callbacks, SysV stack arguments, pNext shadows and retirement"
)
