"""Exercise the game's shader compiler on actual client resources before boot."""

from pathlib import Path
import json
import os
import re
import subprocess
import sys
import zipfile

root = Path(__file__).resolve().parent.parent
binary = Path(sys.argv[1]).resolve()
work = root / "build/minecraft/game-shader-test"
work.mkdir(parents=True, exist_ok=True)
launch = json.loads((root / "build/minecraft/launch-args.json").read_text())
classpath = next(
    x.split("=", 1)[1] for x in launch if x.startswith("-Djava.class.path=")
)
jar = next(x for x in classpath.split(os.pathsep) if "/com/mojang/minecraft/" in x)
with zipfile.ZipFile(jar) as archive:

    def expand(name, chain=()):
        if name in chain:
            raise RuntimeError("Circular shader include: " + name)
        source = archive.read(name).decode()

        def include(match):
            target = match[1].removeprefix("minecraft:")
            return expand("assets/minecraft/shaders/include/" + target, (*chain, name))

        return re.sub(r"^\s*#include\s*<([^>]+)>\s*$", include, source, flags=re.M)

    names = [
        "gui.vsh",
        "gui.fsh",
        "position_tex_color.vsh",
        "position_tex_color.fsh",
        "block.vsh",
        "block.fsh",
        "entity.vsh",
        "entity.fsh",
    ]
    paths = []
    for name in names:
        path = work / name
        path.write_text(expand("assets/minecraft/shaders/core/" + name))
        paths.append(str(path))
guest = work / "compiler-x86_64"
subprocess.run(
    [
        "clang",
        "-target",
        "x86_64-apple-macos11",
        "-O1",
        "-fno-stack-protector",
        "-Wl,-no_fixup_chains",
        str(root / "tests/minecraft_shaders_guest.c"),
        "-o",
        str(guest),
    ],
    check=True,
)
with (work / "run.log").open("w") as log:
    result = subprocess.run(
        [
            str(binary),
            "launch",
            "--host-libs",
            str(guest),
            "--",
            str(root / "build/minecraft/natives/lwjgl/3.4.3+4/x64/libshaderc.dylib"),
            *paths,
        ],
        env=dict(
            os.environ,
            R2_JVM_PROBE="1",
            R2_GUEST_CXX_ABI=str(
                root / "build/minecraft/libcxx/librozetka2-cxxabi.dylib"
            ),
        ),
        stdout=log,
        stderr=subprocess.STDOUT,
        timeout=180,
    )
output = (work / "run.log").read_text()
if (
    result.returncode
    or output.count("PASS Minecraft shader ") != len(paths)
    or "rax = 0 " not in output
):
    raise SystemExit(output)
print(
    "PASS official x86 Shaderc compiled actual Minecraft GUI, textured GUI, block and entity shaders"
)
