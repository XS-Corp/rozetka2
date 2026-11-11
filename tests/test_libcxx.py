"""Run real guest C++ operations and the optional official shader toolchain."""

import argparse
import json
import os
from pathlib import Path
import subprocess
import zipfile

root = Path(__file__).resolve().parent.parent
p = argparse.ArgumentParser()
p.add_argument("translator", type=Path)
p.add_argument("library", type=Path)
p.add_argument("--shaders", action="store_true")
args = p.parse_args()
translator, library = args.translator.resolve(), args.library.resolve()
work = root / "build/minecraft/libcxx-test"
work.mkdir(parents=True, exist_ok=True)
environment = dict(os.environ, R2_JVM_PROBE="1", R2_GUEST_CXX_ABI=str(library))
flags = [
    "-target",
    "x86_64-apple-macos11",
    "-O1",
    "-fno-stack-protector",
    "-Wl,-no_fixup_chains",
]
if args.shaders:
    launch = json.loads((root / "build/minecraft/launch-args.json").read_text())
    cp = next(x.split("=", 1)[1] for x in launch if x.startswith("-Djava.class.path="))
    libraries = []
    for project, filename in [
        ("lwjgl-shaderc", "libshaderc.dylib"),
        ("lwjgl-spvc", "libspirv-cross.dylib"),
    ]:
        archive = next(
            (
                x
                for x in cp.split(os.pathsep)
                if f"/org/lwjgl/{project}/" in x
                and "natives-macos" in Path(x).name
                and "arm64" not in Path(x).name
            ),
            None,
        )
        if archive is None:
            raise SystemExit(
                "Prepare the official client libraries with scripts/run_minecraft.py --prepare-only"
            )
        with zipfile.ZipFile(archive) as z:
            entry = next(x for x in z.namelist() if Path(x).name == filename)
            path = work / filename
            path.write_bytes(z.read(entry))
            libraries.append(str(path))
    binary = work / "shaders-x86_64"
    subprocess.run(
        ["clang", *flags, str(root / "tests/shaders_guest.c"), "-o", str(binary)],
        check=True,
    )
    arguments = libraries
    markers = ["PASS guest Shaderc:", "PASS guest SPIRV-Cross:"]
else:
    binary = work / "stdlib-x86_64"
    subprocess.run(
        [
            "clang++",
            *flags,
            "-std=c++17",
            "-fno-exceptions",
            "-fno-vectorize",
            "-fno-slp-vectorize",
            "-nostdlib++",
            str(root / "tests/libcxx_guest.cpp"),
            "-L" + str(library.parent),
            "-lrozetka2-cxxabi",
            "-Wl,-rpath," + str(library.parent),
            "-o",
            str(binary),
        ],
        check=True,
    )
    arguments = [str(work / "stream.txt")]
    markers = [
        "PASS guest libc++ strings, containers, streams, locale, clocks and concurrent guards"
    ]
logfile = work / ("shaders.log" if args.shaders else "stdlib.log")
with logfile.open("w") as log:
    result = subprocess.run(
        [str(translator), "launch", "--host-libs", str(binary), "--", *arguments],
        env=environment,
        stdout=log,
        stderr=subprocess.STDOUT,
        timeout=120,
    )
output = logfile.read_text()
if (
    result.returncode
    or "rax = 0 " not in output
    or any(x not in output for x in markers)
):
    raise SystemExit(output)
for line in output.splitlines():
    if line.startswith("PASS "):
        print(line)
