"""Real linker-generated weak streams, plus malformed stream regressions."""

from pathlib import Path
import struct
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parent.parent
translator = Path(sys.argv[1]).resolve()


def run(command):
    return subprocess.run(
        [str(x) for x in command], check=True, capture_output=True, text=True
    )


def dyld_info(data):
    offset = 32
    for _ in range(struct.unpack_from("<I", data, 16)[0]):
        kind, size = struct.unpack_from("<II", data, offset)
        if kind == 0x80000022:
            return offset
        offset += size
    raise RuntimeError("fixture has no classic dyld info")


with tempfile.TemporaryDirectory(prefix="rozetka2-weak-") as directory:
    work = Path(directory)
    flags = [
        "clang",
        "-target",
        "x86_64-apple-macos11",
        "-O1",
        "-fno-stack-protector",
        "-Wl,-no_fixup_chains",
    ]
    run(flags + [root / "tests/weak_guest.c", "-o", work / "main"])
    providers = []
    for name, value, strong in [
        ("local", 3, False),
        ("first", 17, False),
        ("second", 99, False),
        ("strong", 42, True),
        ("last", 77, False),
        ("hidden-client", 88, False),
    ]:
        path = work / f"{name}.dylib"
        run(
            flags
            + ["-dynamiclib", f"-DVALUE={value}"]
            + (["-DSTRONG"] if strong else [])
            + [root / "tests/weak_provider.c", "-o", path]
        )
        data = path.read_bytes()
        info = dyld_info(data)
        if not strong and not struct.unpack_from("<I", data, info + 28)[0]:
            raise RuntimeError("linker emitted no weak stream")
        providers.append(path)
    for mode in [
        "export trie WEAK_DEFINITION",
        "nlist N_WEAK_DEF",
        "weak lookup ordinal",
    ]:
        selected = providers
        if mode == "nlist N_WEAK_DEF":
            selected = []
            for path in providers:
                data = bytearray(path.read_bytes())
                struct.pack_into("<II", data, dyld_info(data) + 40, 0, 0)
                target = work / ("symtab-" + path.name)
                target.write_bytes(data)
                selected.append(target)
        if mode == "weak lookup ordinal":
            data = bytearray(providers[4].read_bytes())
            info = dyld_info(data)
            old_offset, old_size = struct.unpack_from("<II", data, info + 16)
            weak_offset, weak_size = struct.unpack_from("<II", data, info + 24)
            # Retain any ordinary imports, then reset state and use the weak
            # lookup ordinal as used by newer import formats.
            stream = bytes(data[old_offset : old_offset + old_size]).rstrip(b"\0")
            stream += b"\x60\x00\x3d" + data[weak_offset : weak_offset + weak_size]
            struct.pack_into("<II", data, info + 16, len(data), len(stream))
            struct.pack_into("<II", data, info + 24, 0, 0)
            data += stream
            target = work / "weak-ordinal.dylib"
            target.write_bytes(data)
            selected = list(providers)
            selected[4] = target
        result = run(
            [translator, "launch", "--host-libs", work / "main", "--", *selected]
        )
        if (
            "R2_WEAK canonical functions, data, addends and visibility: OK"
            not in result.stdout
            or "rax = 42" not in result.stdout
        ):
            raise RuntimeError(result.stdout + result.stderr)
        print("PASS weak coalescing " + mode)
    streams = [
        (b"\x40_missing_definition\0\x71\xff\xff\x7f\x90\0", "outside segment"),
        (b"\x11\0", "ordinal opcode in weak bind stream"),
        (b"\x40unterminated", "unterminated symbol"),
    ]
    for index, (stream, expected) in enumerate(streams):
        data = bytearray(providers[0].read_bytes())
        struct.pack_into("<II", data, dyld_info(data) + 24, len(data), len(stream))
        data += stream
        path = work / f"malformed-{index}.dylib"
        path.write_bytes(data)
        result = subprocess.run(
            [
                str(translator),
                "launch",
                "--host-libs",
                str(work / "main"),
                "--",
                str(path),
                *map(str, providers[1:]),
            ],
            capture_output=True,
            text=True,
        )
        if result.returncode == 0 or expected not in result.stderr:
            raise RuntimeError(
                "malformed weak stream accepted or wrong error: "
                + result.stdout
                + result.stderr
            )
    print("PASS malformed weak streams: bounds, ordinal and truncated symbol")
