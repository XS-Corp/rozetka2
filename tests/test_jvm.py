"""Run the real x86 JVM only through rozetka2; never execute it via the OS."""

import argparse
import hashlib
import os
from pathlib import Path
import subprocess


def require(condition, message):
    if not condition:
        raise SystemExit("FAIL " + message)


parser = argparse.ArgumentParser()
parser.add_argument("translator", type=Path)
parser.add_argument("probe", type=Path)
mode = parser.add_mutually_exclusive_group()
mode.add_argument("--create", action="store_true")
mode.add_argument("--image", action="store_true")
args = parser.parse_args()
work = Path(__file__).resolve().parent.parent / "build" / "minecraft"
work.mkdir(parents=True, exist_ok=True)
command = [
    str(args.translator.resolve()),
    "launch",
    "--host-libs",
    str(args.probe.resolve()),
]
if args.create:
    command += ["--", "create"]
if args.image:
    jdk = Path((work / "jdk-home.txt").read_text().strip())
    (work / "Object.class").unlink(missing_ok=True)
    command += [
        "--",
        "image",
        str(jdk / "lib/libjimage.dylib"),
        str(jdk / "lib/modules"),
        str(work / "Object.class"),
    ]
environment = dict(os.environ, R2_JVM_PROBE="1")
guest_abi = work / "cxxabi" / "librozetka2-cxxabi.dylib"
require(guest_abi.is_file(), "build the guest C++ ABI first: make cxxabi")
environment.setdefault("R2_GUEST_CXX_ABI", str(guest_abi))
result = subprocess.run(
    command,
    env=environment,
    stdout=subprocess.PIPE,
    stderr=subprocess.STDOUT,
    text=True,
    timeout=60,
)
log = work / (
    "create-test.log"
    if args.create
    else "image-test.log" if args.image else "query-test.log"
)
log.write_text(result.stdout)
print(result.stdout, end="")
require(result.returncode == 0, f"x86 JVM failed; see {log}")
require("R2_JVM default_args status=0" in result.stdout, "JNI default arguments")
require("R2_JVM created_vms status=0 count=0" in result.stdout, "JNI created-VM query")
require("rax = 0 " in result.stdout, "guest exit status")
if args.create:
    require("R2_JVM create_vm status=0" in result.stdout, "VM creation")
    require("R2_JVM java.version=" in result.stdout, "Java code execution")
if args.image:
    data = (work / "Object.class").read_bytes()
    require(
        "R2_JIMAGE java/lang/Object.class" in result.stdout,
        "guest JIMAGE resource lookup",
    )
    require(
        len(data) > 100 and data[:4] == bytes.fromhex("cafebabe"),
        "Java class file magic",
    )
    require(int.from_bytes(data[6:8], "big") >= 69, "JDK 25 or newer class file")
    print(
        "PASS x86 libjimage read Java class sha256=" + hashlib.sha256(data).hexdigest()
    )
else:
    print(
        "PASS real x86 JVM "
        + (
            "creation and Java System.getProperty"
            if args.create
            else "JNI queries, initializers and exit callbacks"
        )
    )
