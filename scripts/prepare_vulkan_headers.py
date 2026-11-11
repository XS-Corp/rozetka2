#!/usr/bin/env python3
"""Cache the pinned Khronos public headers for optional x86 graphics probes."""

from concurrent.futures import ThreadPoolExecutor
from pathlib import Path, PurePosixPath
import re
import urllib.request

root = Path(__file__).resolve().parent.parent / "build/minecraft/graphics-abi/include"
base = "https://raw.githubusercontent.com/KhronosGroup/Vulkan-Headers/9fbc2a782ee3f8a4db64e7d991e23da75e893f44/include/"


def fetch(name):
    path = root / name
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.is_file():
        with urllib.request.urlopen(base + name, timeout=30) as response:
            data = response.read()
        path.write_bytes(data)
    if name == "vulkan/vulkan.h":
        return ["vulkan/vulkan_core.h", "vulkan/vk_platform.h"]
    return [
        (
            str(PurePosixPath(name).parent / dependency)
            if "/" not in dependency
            else dependency
        )
        for dependency in re.findall(r'#include "([^"]+)"', path.read_text())
    ]


seen = set()
pending = {"vulkan/vulkan.h"}
with ThreadPoolExecutor(max_workers=4) as pool:
    while pending:
        batch = pending - seen
        seen.update(batch)
        pending = set()
        for dependencies in pool.map(fetch, sorted(batch)):
            pending.update(set(dependencies) - seen)
print("Prepared", len(seen), "pinned Khronos headers")
