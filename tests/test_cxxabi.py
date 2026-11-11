"""Run RTTI and virtual dispatch with x86 guest vtables and implementations."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parent.parent
translator, library = map(lambda p: Path(p).resolve(), sys.argv[1:3])
with tempfile.TemporaryDirectory(prefix="rozetka2-cxxabi-") as directory:
    binary = Path(directory) / "guest"
    subprocess.run(
        [
            "clang++",
            "-target",
            "x86_64-apple-macos11",
            "-std=c++17",
            "-O1",
            "-fno-exceptions",
            "-fno-stack-protector",
            "-fno-vectorize",
            "-fno-slp-vectorize",
            "-nostdlib++",
            str(root / "tests/cxxabi_guest.cpp"),
            "-L" + str(library.parent),
            "-lrozetka2-cxxabi",
            "-Wl,-rpath," + str(library.parent),
            "-Wl,-no_fixup_chains",
            "-o",
            str(binary),
        ],
        check=True,
    )
    environment = dict(os.environ)
    environment.pop("R2_JVM_PROBE", None)
    environment.pop("R2_GUEST_CXX_ABI", None)
    if "--stdlib" in sys.argv[3:]:
        environment["R2_JVM_PROBE"] = "1"
    result = subprocess.run(
        [str(translator), "launch", "--host-libs", str(binary)],
        env=environment,
        capture_output=True,
        text=True,
        timeout=30,
    )
    print(result.stdout, end="")
    if (
        result.returncode
        or "R2_CXXABI x86 RTTI, multiple/virtual inheritance and allocation: OK"
        not in result.stdout
        or "rax = 42" not in result.stdout
    ):
        raise SystemExit("FAIL guest C++ ABI: " + result.stderr)
print("PASS guest libc++abi RTTI, vtables, casts, new/delete and nothrow allocation")
