"""Validate native OpenGL rendering invoked by an x86 guest through typed stubs."""

from pathlib import Path
import argparse
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("translator", type=Path)
parser.add_argument("guest", type=Path)
parser.add_argument("--game-shaders", action="store_true")
args = parser.parse_args()
root = Path(__file__).resolve().parent.parent
command = [
    str(args.translator.resolve()),
    "launch",
    "--host-libs",
    str(args.guest.resolve()),
]
if args.game_shaders:
    command += [
        "--",
        str(root / "build/minecraft/game-shader-test/gui.vsh"),
        str(root / "build/minecraft/game-shader-test/gui.fsh"),
    ]
result = subprocess.run(command, capture_output=True, text=True, timeout=30)
if (
    result.returncode
    or "PASS guest OpenGL GPU readback:" not in result.stdout
    or "rax = 0 " not in result.stdout
):
    raise SystemExit(result.stdout + result.stderr)
if (
    args.game_shaders
    and "PASS original Minecraft OpenGL GUI shaders compiled, linked and rendered 128 pixels"
    not in result.stdout
):
    raise SystemExit(result.stdout + result.stderr)
for line in result.stdout.splitlines():
    if line.startswith("PASS "):
        print(line)
