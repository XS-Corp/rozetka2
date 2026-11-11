"""Present a real GPU frame through the x86 ABI and verify all readback pixels."""

from pathlib import Path
import os
import subprocess
import sys

root = Path(__file__).resolve().parent.parent
binary = Path(sys.argv[1]).resolve()
guest = root / "build/vulkan-frame-x86_64"
subprocess.run(
    [sys.executable, str(root / "scripts/prepare_vulkan_headers.py")], check=True
)
subprocess.run(
    [
        "clang",
        "-target",
        "x86_64-apple-macos11",
        "-O1",
        "-fno-stack-protector",
        "-Wl,-no_fixup_chains",
        "-I" + str(root / "build/minecraft/graphics-abi/include"),
        str(root / "tests/vulkan_frame_guest.c"),
        "-o",
        str(guest),
    ],
    check=True,
)
arguments = (
    [
        "--",
        str(root / "build/minecraft/game-shader-test/gui.vsh.spv"),
        str(root / "build/minecraft/game-shader-test/gui.fsh.spv"),
    ]
    if "--game-shaders" in sys.argv
    else []
)
result = subprocess.run(
    [str(binary), "launch", "--host-libs", str(guest), *arguments],
    env=dict(
        os.environ,
        R2_HOST_SDL3="/opt/homebrew/opt/sdl3/lib/libSDL3.dylib",
        R2_HOST_VULKAN=str(root / "build/minecraft/host-driver/libMoltenVK.dylib"),
    ),
    capture_output=True,
    text=True,
    timeout=30,
)
if (
    result.returncode
    or "PASS Vulkan swapchain, GPU clear/copy/present" not in result.stdout
    or "rax = 0 " not in result.stdout
):
    raise SystemExit(result.stdout + result.stderr)
if arguments and "PASS actual Minecraft GUI shaders:" not in result.stdout:
    raise SystemExit(result.stdout + result.stderr)
print(
    "PASS actual Minecraft GUI shaders rendered on Vulkan GPU with framebuffer verification"
    if arguments
    else "PASS x86 Vulkan swapchain, GPU command submission/presentation and exact framebuffer readback"
)
