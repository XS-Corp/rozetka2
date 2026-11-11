"""Decode Minecraft textures and sounds with its actual x86 STB library."""

from pathlib import Path
import json
import os
import subprocess
import sys
import zipfile
import zlib

root = Path(__file__).resolve().parent.parent
binary = Path(sys.argv[1]).resolve()
work = root / "build/minecraft/stb-test"
work.mkdir(parents=True, exist_ok=True)
args = json.loads((root / "build/minecraft/launch-args.json").read_text())
cp = next(x.split("=", 1)[1] for x in args if x.startswith("-Djava.class.path="))
jar = next(x for x in cp.split(os.pathsep) if "/com/mojang/minecraft/" in x)
with zipfile.ZipFile(jar) as archive:
    png = archive.read("assets/minecraft/textures/block/stone.png")
assert png[16:29] == bytes.fromhex(
    "00000010000000100800000000"
), "fixture must be 16x16 8-bit grayscale"
idat = b""
offset = 8
while offset < len(png):
    size = int.from_bytes(png[offset : offset + 4])
    kind = png[offset + 4 : offset + 8]
    if kind == b"IDAT":
        idat += png[offset + 8 : offset + 8 + size]
    offset += 12 + size
raw = zlib.decompress(idat)
previous = [0] * 16
rgba = []
for y in range(16):
    mode = raw[y * 17]
    row = list(raw[y * 17 + 1 : y * 17 + 17])
    for x in range(16):
        a = row[x - 1] if x else 0
        b = previous[x]
        c = previous[x - 1] if x else 0
        if mode == 1:
            predict = a
        elif mode == 2:
            predict = b
        elif mode == 3:
            predict = (a + b) // 2
        elif mode == 4:
            p = a + b - c
            distances = (abs(p - a), abs(p - b), abs(p - c))
            predict = (a, b, c)[distances.index(min(distances))]
        else:
            assert mode == 0
            predict = 0
        row[x] = (row[x] + predict) & 255
        rgba.extend([row[x], row[x], row[x], 255])
    previous = row
hash_value = 14695981039346656037
for byte in rgba:
    hash_value = ((hash_value ^ byte) * 1099511628211) & ((1 << 64) - 1)
texture = work / "stone.png"
texture.write_bytes(png)
assets = Path(args[args.index("--assetsDir") + 1])
index = json.loads(
    (assets / "indexes" / f"{args[args.index('--assetIndex')+1]}.json").read_text()
)["objects"]
sound_name = "minecraft/sounds/random/click.ogg"
sound = index[sound_name]
audio = assets / "objects" / sound["hash"][:2] / sound["hash"]
guest = work / "stb-x86_64"
subprocess.run(
    [
        "clang",
        "-target",
        "x86_64-apple-macos11",
        "-O1",
        "-fno-stack-protector",
        "-Wl,-no_fixup_chains",
        str(root / "tests/stb_guest.c"),
        "-o",
        str(guest),
    ],
    check=True,
)
command = [
    str(binary),
    "launch",
    "--host-libs",
    str(guest),
    "--",
    str(root / "build/minecraft/natives/lwjgl/3.4.3+4/x64/liblwjgl_stb.dylib"),
    str(texture),
    f"{hash_value:x}",
    str(audio),
]
with (work / "run.log").open("w") as log:
    result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=90)
output = (work / "run.log").read_text()
if (
    result.returncode
    or "PASS real x86 STB Vorbis:" not in output
    or "PASS real x86 STB: all stone texture" not in output
    or "rax = 0 " not in output
):
    raise SystemExit(output)
print(
    "PASS actual Minecraft x86 STB: exact texture pixels and decoded game Vorbis PCM ("
    + sound_name
    + ")"
)
