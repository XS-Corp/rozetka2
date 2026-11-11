#!/usr/bin/env python3
"""Bundle the native translator for Cocoa window identification and launch."""

from pathlib import Path
import argparse
import plistlib
import shutil
import subprocess
import sys

root = Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser()
parser.add_argument("--c2", action="store_true")
options = parser.parse_args()
name = "Minecraft-rozetka2" + ("-C2" if options.c2 else "")
profile = "game-app" + ("-c2" if options.c2 else "")
game = root / "build/minecraft" / profile
game.mkdir(parents=True, exist_ok=True)
settings = game / "options.txt"
if options.c2 and not settings.exists():
    settings.write_text(
        "renderDistance:2\nsimulationDistance:5\nmaxFps:30\nfullscreen:false\n"
    )
app = root / ("build/" + name + ".app")
contents = app / "Contents"
macos = contents / "MacOS"
macos.mkdir(parents=True, exist_ok=True)
translator = macos / "rozetka2"
temporary = macos / "rozetka2-new"
shutil.copy2(root / "build/rozetka2", temporary)
temporary.replace(translator)
# Retain debug types even after subsequent builds replace the object files.
subprocess.run(["dsymutil", str(translator), "-o", str(app) + ".dSYM"], check=True)
with (contents / "Info.plist").open("wb") as file:
    plistlib.dump(
        {
            "CFBundleIdentifier": "org.rozetka2.minecraft"
            + (".c2" if options.c2 else ""),
            "CFBundleName": name,
            "CFBundleDisplayName": name,
            "CFBundleExecutable": "start-minecraft",
            "CFBundlePackageType": "APPL",
            "CFBundleVersion": "1",
            "CFBundleShortVersionString": "0.1",
            "LSMinimumSystemVersion": "11.0",
            "NSHighResolutionCapable": True,
            "NSPrincipalClass": "NSApplication",
        },
        file,
    )
log = root / (
    "build/minecraft-signals/minecraft-app" + ("-c2" if options.c2 else "") + ".log"
)
log.parent.mkdir(parents=True, exist_ok=True)
command = [
    sys.executable,
    str(root / "scripts/run_minecraft.py"),
    "--cached",
    "--exec",
    "--translator",
    str(translator),
    "--game-dir",
    str(root / "build/minecraft" / profile),
    *(["--c2"] if options.c2 else []),
]
entry = macos / "start-minecraft"
resources = contents / "Resources"
resources.mkdir(parents=True, exist_ok=True)
(resources / "launch.bin").write_bytes(
    b"\0".join(str(value).encode() for value in [log, *command]) + b"\0"
)
temporary = macos / "start-minecraft-new"
subprocess.run(
    [
        "clang",
        "-target",
        "arm64-apple-macos11",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-O2",
        str(root / "ports/minecraft_launcher.c"),
        "-o",
        str(temporary),
    ],
    check=True,
)
temporary.replace(entry)
print(app)
