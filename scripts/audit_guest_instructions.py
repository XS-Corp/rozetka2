#!/usr/bin/env python3
"""Compare the decoder with compiler-disassembled instructions in guest libraries.

This reports static coverage, including optional CPU paths and deliberate traps.
It does not imply that every reported instruction is needed by the current CPU.
"""

import argparse
from bisect import bisect_left
from collections import Counter
import json
from pathlib import Path
import re
import struct
import subprocess
import tempfile

root = Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser()
parser.add_argument("images", type=Path, nargs="+")
parser.add_argument(
    "--output",
    type=Path,
    default=root / "build/minecraft-signals/instruction-audit.json",
)
options = parser.parse_args()
helper = root / "build/audit-instructions"
subprocess.run(
    [
        "cc",
        "-I" + str(root / "include"),
        "-I" + str(root / "src"),
        "-std=c17",
        "-O2",
        str(root / "tests/audit_instructions.c"),
        str(root / "src/decode.c"),
        str(root / "src/util.c"),
        "-o",
        str(helper),
    ],
    check=True,
)


def text_section(data):
    magic = data[:4]
    if magic in (
        bytes.fromhex("cafebabe"),
        bytes.fromhex("bebafeca"),
        bytes.fromhex("cafebabf"),
        bytes.fromhex("bfbafeca"),
    ):
        endian = ">" if magic[0] == 0xCA else "<"
        wide = magic in (bytes.fromhex("cafebabf"), bytes.fromhex("bfbafeca"))
        count = struct.unpack_from(endian + "I", data, 4)[0]
        for i in range(count):
            p = 8 + i * (32 if wide else 20)
            cpu = struct.unpack_from(endian + "I", data, p)[0]
            if cpu == 0x1000007:
                offset, size = struct.unpack_from(
                    endian + ("QQ" if wide else "II"), data, p + 8
                )
                return text_section(data[offset : offset + size])
        raise ValueError("No x86_64 slice")
    if data[:4] != bytes.fromhex("cffaedfe"):
        raise ValueError("Not little-endian Mach-O 64")
    commands = struct.unpack_from("<I", data, 16)[0]
    position = 32
    text = None
    embedded = []
    for _ in range(commands):
        kind, size = struct.unpack_from("<II", data, position)
        if kind == 0x19:
            count = struct.unpack_from("<I", data, position + 64)[0]
            for i in range(count):
                p = position + 72 + i * 80
                if (
                    data[p : p + 16].rstrip(b"\0") == b"__text"
                    and data[p + 16 : p + 32].rstrip(b"\0") == b"__TEXT"
                ):
                    address, length, offset = struct.unpack_from("<QQI", data, p + 32)
                    text = (address, offset, data[offset : offset + length])
        if kind == 0x29:
            offset, length = struct.unpack_from("<II", data, position + 8)
            embedded.extend(
                struct.unpack_from("<IHH", data, offset + i)[:2]
                for i in range(0, length, 8)
            )
        position += size
    if text:
        address, file_offset, code = text
        patched = bytearray(data)
        for offset, length in embedded:
            patched[offset : offset + length] = b"\x90" * length
        return (
            address,
            code,
            [
                (
                    address + offset - file_offset,
                    address + offset - file_offset + length,
                )
                for offset, length in embedded
            ],
            patched,
        )
    raise ValueError("No __TEXT,__text")


reports = []
for image in options.images:
    base, code, embedded, patched = text_section(image.read_bytes())
    # Data-in-code tables otherwise desynchronize otool past the table's end.
    # NOPs preserve all addresses; only the analysis copy is changed, never run.
    prefixes = {
        "lock",
        "rep",
        "repe",
        "repne",
        "repz",
        "repnz",
        "data16",
        "addr32",
        "rex64",
        "rex.W",
    }
    embedded.sort()
    starts = [interval[0] for interval in embedded]
    records = bytearray()
    by_address = {}
    missing = []
    statistics = {"instructions": 0, "undecoded": 0}

    def flush():
        if not records:
            return
        result = subprocess.run(
            [str(helper)], input=records, capture_output=True, check=True
        )
        for line in result.stdout.decode().splitlines():
            address, detail = line.split(" ", 1)
            address = int(address, 16)
            mnemonic, symbol = by_address[address]
            missing.append(
                {
                    "address": hex(address),
                    "instruction": mnemonic,
                    "function": symbol,
                    "detail": detail,
                }
            )
        records.clear()
        by_address.clear()

    def append(entry, end):
        address, mnemonic, symbol = entry
        length = end - address
        if mnemonic.startswith(".byte"):
            statistics["undecoded"] += 1
        if (
            not 1 <= length <= 15
            or not base <= address < base + len(code)
            or mnemonic.startswith(".")
        ):
            return
        index = bisect_left(starts, end) - 1
        if index >= 0 and embedded[index][1] > address:
            return
        data = code[address - base : address - base + length]
        if len(data) != length:
            return
        records.extend(struct.pack("<QB", address, length) + data.ljust(15, b"\0"))
        by_address[address] = (mnemonic, symbol)
        statistics["instructions"] += 1
        if len(by_address) >= 65536:
            flush()

    # Keep the disassembly on disk and bound the decoder batches. libjvm has
    # millions of instructions; retaining every Python row would consume GiB.
    with tempfile.TemporaryDirectory(prefix="r2-instruction-audit-") as directory:
        analysis = Path(directory) / image.name
        analysis.write_bytes(patched)
        assembly = Path(directory) / "assembly.txt"
        with assembly.open("w") as output:
            subprocess.run(
                ["xcrun", "otool", "-arch", "x86_64", "-tvV", str(analysis)],
                stdout=output,
                stderr=subprocess.PIPE,
                check=True,
            )
        previous = None
        prefix = None
        symbol = ""
        with assembly.open() as lines:
            for line in lines:
                line = line.rstrip("\n")
                match = re.match(r"^([0-9a-f]{8,16})\s+(\S+)\s*(.*)", line)
                if match:
                    address = int(match[1], 16)
                    mnemonic = match[2]
                    # otool emits LOCK separately, REP MOVS on one line.
                    if (
                        mnemonic in {"rep", "repe", "repne", "repz", "repnz"}
                        and match[3].strip()
                    ):
                        mnemonic += ":" + match[3].split()[0]
                    if mnemonic in prefixes:
                        if prefix is None:
                            prefix = address
                        continue
                    address = prefix if prefix is not None else address
                    prefix = None
                    if previous is not None:
                        append(previous, address)
                    previous = (address, mnemonic, symbol)
                elif line.endswith(":") and not line.startswith(("(__", "/")):
                    symbol = line[:-1]
        if previous is not None:
            append(previous, base + len(code))
        flush()
    undecoded = statistics["undecoded"]
    counts = Counter(entry["instruction"] for entry in missing)
    print(
        f"{image.name}: Decoded {statistics['instructions']} instructions; {len(missing)} unsupported or inconsistent"
    )
    print("  " + ", ".join(f"{name}={count}" for name, count in sorted(counts.items())))
    if undecoded:
        print(
            f"  WARNING: native disassembler emitted {undecoded} undecoded byte rows; embedded data may cause false instruction boundaries"
        )
    reports.append(
        {
            "image": str(image.resolve()),
            "instruction_count": statistics["instructions"],
            "native_undecoded_rows": undecoded,
            "missing": missing,
        }
    )
output = options.output
output.parent.mkdir(parents=True, exist_ok=True)
output.write_text(json.dumps(reports, indent=2))
print("Details:", output)
