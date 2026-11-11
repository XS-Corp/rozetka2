"""Require correct Java arithmetic interpreted and in forced x86 C1/C2 code."""

import argparse
import math
import os
from pathlib import Path
import re
import struct
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("translator", type=Path)
parser.add_argument("launcher", type=Path)
args = parser.parse_args()
root = Path(__file__).resolve().parent.parent
work = root / "build/minecraft/hot-test"
work.mkdir(parents=True, exist_ok=True)
pool = []


def utf(value):
    data = value.encode()
    pool.append(b"\1" + struct.pack(">H", len(data)) + data)
    return len(pool)


def cls(value):
    name = utf(value)
    pool.append(b"\7" + struct.pack(">H", name))
    return len(pool)


def reference(tag, owner, name, descriptor):
    n = utf(name)
    t = utf(descriptor)
    pool.append(b"\x0c" + struct.pack(">HH", n, t))
    nt = len(pool)
    pool.append(bytes([tag]) + struct.pack(">HH", owner, nt))
    return len(pool)


this = cls("R2Hot")
base = cls("java/lang/Object")
system = cls("java/lang/System")
printer = cls("java/io/PrintStream")
out = reference(9, system, "out", "Ljava/io/PrintStream;")
println = reference(10, printer, "println", "(I)V")
code_name = utf("Code")
name = utf("main")
descriptor = utf("([Ljava/lang/String;)V")
worker_name = utf("run")
worker_descriptor = utf("(I)I")
worker_ref = reference(10, this, "run", "(I)I")
iterations = 100000
pool.append(b"\3" + struct.pack(">i", iterations))
limit = len(pool)
# run(int n): int result=0; for(int i=0;i<n;i++) result+=i; return result;
# Version 49 permits inferred verifier frames; no javac or host JVM is used.
worker = bytes(
    [
        3,
        0x3C,
        3,
        0x3D,
        0x1B,
        0x1A,
        0xA2,
        0,
        13,
        0x1C,
        0x1B,
        0x60,
        0x3D,
        0x84,
        1,
        1,
        0xA7,
        0xFF,
        0xF4,
        0x1C,
        0xAC,
    ]
)
main = (
    bytes([0xB2])
    + struct.pack(">H", out)
    + bytes([0x12, limit, 0xB8])
    + struct.pack(">H", worker_ref)
    + bytes([0xB6])
    + struct.pack(">H", println)
    + bytes([0xB1])
)
double_name = utf("remDouble")
double_descriptor = utf("(DD)D")
double_ref = reference(10, this, "remDouble", "(DD)D")
float_name = utf("remFloat")
float_descriptor = utf("(FF)F")
float_ref = reference(10, this, "remFloat", "(FF)F")
double_bits = reference(10, cls("java/lang/Double"), "doubleToRawLongBits", "(D)J")
float_bits = reference(10, cls("java/lang/Float"), "floatToRawIntBits", "(F)I")
println_long = reference(10, printer, "println", "(J)V")
math_expected = []
main = main[:-1]
for wide, pairs in [
    (
        True,
        [
            (13.5, 2.0),
            (-13.5, 2.0),
            (-0.0, 3.0),
            (float.fromhex("0x1.fffffffffffffp1023"), 0.3),
        ],
    ),
    (
        False,
        [
            (13.5, 2.0),
            (-13.5, 2.0),
            (-0.0, 3.0),
            (float.fromhex("0x1.fffffep127"), 0.1),
        ],
    ),
]:
    for left, right in pairs:
        main += b"\xb2" + struct.pack(">H", out)
        values = []
        for value in (left, right):
            packed = struct.pack(">d" if wide else ">f", value)
            values.append(struct.unpack(">d" if wide else ">f", packed)[0])
            pool.append((b"\6" if wide else b"\4") + packed)
            index = len(pool)
            if wide:
                pool.append(b"")  # JVM double constants reserve two pool slots.
            main += (b"\x14" if wide else b"\x13") + struct.pack(">H", index)
        main += b"\xb8" + struct.pack(">H", double_ref if wide else float_ref)
        main += b"\xb8" + struct.pack(">H", double_bits if wide else float_bits)
        main += b"\xb6" + struct.pack(">H", println_long if wide else println)
        remainder = math.fmod(*values)
        math_expected.append(
            str(
                struct.unpack(
                    ">q" if wide else ">i",
                    struct.pack(">d" if wide else ">f", remainder),
                )[0]
            )
        )
noise_name = utf("sample")
noise_descriptor = utf("(I)D")
noise_ref = reference(10, this, "sample", "(I)D")
constants = []
for value in (0.125, 0.75, 0.03125):
    pool.append(b"\6" + struct.pack(">d", value))
    constants.append(len(pool))
    pool.append(b"")
noise = bytearray(b"\x14" + struct.pack(">H", constants[0]) + b"\x49\x03\x3c")
loop = len(noise)
noise += b"\x1b\x1a"
branch = len(noise)
noise += b"\xa2\0\0"
noise += (
    b"\x28\x14"
    + struct.pack(">H", constants[1])
    + b"\x6b\x1b\x10\x0f\x7e\x10\x07\x64\x87\x14"
    + struct.pack(">H", constants[2])
    + b"\x6b\x63\x49\x84\x01\x01"
)
back = len(noise)
noise += b"\xa7" + struct.pack(">h", loop - back)
struct.pack_into(">h", noise, branch + 1, len(noise) - branch)
noise += b"\x28\xaf"
main += (
    b"\xb2"
    + struct.pack(">H", out)
    + bytes([0x12, limit, 0xB8])
    + struct.pack(">H", noise_ref)
)
main += (
    b"\xb8"
    + struct.pack(">H", double_bits)
    + b"\xb6"
    + struct.pack(">H", println_long)
    + b"\xb1"
)
value = 0.125
for i in range(iterations):
    value = value * 0.75 + ((i & 15) - 7) * 0.03125
math_expected.append(str(struct.unpack(">q", struct.pack(">d", value))[0]))


def method(n, d, locals_count, code, stack=2):
    attribute = (
        struct.pack(">HHI", stack, locals_count, len(code))
        + code
        + struct.pack(">HH", 0, 0)
    )
    return (
        struct.pack(">HHHH", 9, n, d, 1)
        + struct.pack(">HI", code_name, len(attribute))
        + attribute
    )


header = (
    bytes.fromhex("cafebabe")
    + struct.pack(">HHH", 0, 49, len(pool) + 1)
    + b"".join(pool)
    + struct.pack(">HHHHHH", 0x21, this, base, 0, 0, 5)
)
(work / "R2Hot.class").write_bytes(
    header
    + method(name, descriptor, 1, main, 5)
    + method(worker_name, worker_descriptor, 3, worker)
    + method(double_name, double_descriptor, 4, bytes([0x26, 0x28, 0x73, 0xAF]), 4)
    + method(float_name, float_descriptor, 2, bytes([0x22, 0x23, 0x72, 0xAE]))
    + method(noise_name, noise_descriptor, 4, noise, 6)
    + struct.pack(">H", 0)
)
environment = dict(os.environ, R2_JVM_PROBE="1")
environment.setdefault(
    "R2_GUEST_CXX_ABI", str(root / "build/minecraft/cxxabi/librozetka2-cxxabi.dylib")
)
expected = str((iterations * (iterations - 1) // 2) & 0xFFFFFFFF)
for mode in ("interpreter", "compiled", "c2"):
    flags = (
        []
        if mode == "interpreter"
        else [
            "-Xcomp",
            *(
                ["-XX:-TieredCompilation"]
                if mode == "c2"
                else ["-XX:TieredStopAtLevel=1"]
            ),
            "-XX:CICompilerCount=1",
            "-XX:+UnlockDiagnosticVMOptions",
            "-XX:-ImplicitNullChecks",
            "-XX:+PrintCompilation",
            *[
                "-XX:CompileCommand=compileonly,R2Hot::" + method
                for method in ("run", "remDouble", "remFloat", "sample")
            ],
        ]
    )
    command = [
        str(args.translator.resolve()),
        "launch",
        "--host-libs",
        str(args.launcher.resolve()),
        "--",
        *flags,
        f"-Djava.class.path={work}",
        "--",
        "R2Hot",
    ]
    result = subprocess.run(
        command,
        env=environment,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        timeout=120,
    )
    (work / f"{mode}.log").write_text(result.stdout)
    valid = (
        result.returncode == 0
        and all(
            value in result.stdout.splitlines() for value in [expected, *math_expected]
        )
        and "rax = 0 " in result.stdout
        and "R2_JAVA invoking_main" in result.stdout
    )
    if mode != "interpreter":
        for target in ("run", "remDouble", "remFloat", "sample"):
            valid = valid and any(
                "R2Hot::" + target in line and re.match(r"^\s*\d+\s+\d+\s+", line)
                for line in result.stdout.splitlines()
            )
            valid = valid and not any(
                "R2Hot::" + target in line and "uncommon trap" in line
                for line in result.stdout.splitlines()
            )
    if not valid:
        raise SystemExit("FAIL " + mode + " Java loop:\n" + result.stdout[-12000:])
    print(
        f"PASS real x86 JVM {mode}: sum={expected}, exact dense double loop and float/double remainders"
        + (
            " (forced " + ("C2" if mode == "c2" else "C1") + " compilation)"
            if mode != "interpreter"
            else ""
        )
    )
