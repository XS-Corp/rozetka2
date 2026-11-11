#!/usr/bin/env python3
"""Prepare the official client and launch its demo through the x86 guest JVM.

Uses existing Prism library/assets caches; never reads account files or worlds.
"""

import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import urllib.request

ROOT = Path(__file__).resolve().parent.parent
WORK = ROOT / "build/minecraft"
PRISM = Path.home() / "Library/Application Support/PrismLauncher"


def digest(path):
    return hashlib.sha1(path.read_bytes()).hexdigest()


def download(item, destination, cached=None):
    for path in (destination, cached):
        if (
            path is not None
            and path.is_file()
            and path.stat().st_size == item["size"]
            and digest(path) == item["sha1"]
        ):
            return path
    destination.parent.mkdir(parents=True, exist_ok=True)
    data = urllib.request.urlopen(item["url"], timeout=60).read()
    if len(data) != item["size"] or hashlib.sha1(data).hexdigest() != item["sha1"]:
        raise RuntimeError("Download verification failed: " + item["url"])
    destination.write_bytes(data)
    return destination


def allowed(library):
    rules = library.get("rules")
    if rules is None:
        return True
    result = False
    for rule in rules:
        conditions = rule.get("os", {})
        match = all(
            re.search(
                pattern,
                {"name": "osx", "arch": "x86_64", "version": "26.0"}.get(key, ""),
            )
            for key, pattern in conditions.items()
        )
        if match:
            result = rule["action"] == "allow"
    return result


def prepare():
    WORK.mkdir(parents=True, exist_ok=True)
    manifest = json.loads(
        urllib.request.urlopen(
            "https://piston-meta.mojang.com/mc/game/version_manifest_v2.json"
        ).read()
    )
    release = manifest["latest"]["release"]
    entry = next(x for x in manifest["versions"] if x["id"] == release)
    raw = urllib.request.urlopen(entry["url"]).read()
    if hashlib.sha1(raw).hexdigest() != entry["sha1"]:
        raise RuntimeError("Version manifest verification failed")
    (WORK / "version.json").write_bytes(raw)
    version = json.loads(raw)
    classpath = []
    for library in version["libraries"]:
        if not allowed(library):
            continue
        # The official manifest lists both Mac architectures. Exclude ARM native
        # jars so every native component selected for this run is x86_64.
        if any(
            tag in library["name"] for tag in (":natives-macos-arm64", ":osx-aarch_64")
        ):
            continue
        item = library["downloads"]["artifact"]
        classpath.append(
            str(
                download(
                    item,
                    WORK / "libraries" / item["path"],
                    PRISM / "libraries" / item["path"],
                )
            )
        )
    client = version["downloads"]["client"]
    local = (
        PRISM
        / "libraries/com/mojang/minecraft"
        / release
        / f"minecraft-{release}-client.jar"
    )
    classpath.append(
        str(download(client, WORK / f"minecraft-{release}-client.jar", local))
    )
    index = version["assetIndex"]
    assets = PRISM / "assets"
    if not assets.is_dir():
        assets = WORK / "assets"
    index_path = download(index, assets / "indexes" / f'{index["id"]}.json')
    objects = json.loads(index_path.read_text())["objects"]

    def asset(item):
        sha = item["hash"]
        path = assets / "objects" / sha[:2] / sha
        if path.is_file() and path.stat().st_size == item["size"]:
            return
        download(
            dict(
                item,
                sha1=sha,
                url=f"https://resources.download.minecraft.net/{sha[:2]}/{sha}",
            ),
            path,
        )

    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
        list(pool.map(asset, objects.values()))
    driver_item = next(
        x["downloads"]["artifact"]
        for x in version["libraries"]
        if x["name"].startswith("org.lwjgl:lwjgl-vulkan:")
        and x["name"].endswith(":natives-macos-arm64")
    )
    driver_archive = download(driver_item, WORK / "host-driver" / driver_item["path"])
    import zipfile

    with zipfile.ZipFile(driver_archive) as archive:
        name = next(n for n in archive.namelist() if n.endswith("/libMoltenVK.dylib"))
        driver = WORK / "host-driver/libMoltenVK.dylib"
        driver.write_bytes(archive.read(name))
    game = WORK / "game"
    game.mkdir(exist_ok=True)
    natives = WORK / "natives"
    natives.mkdir(exist_ok=True)
    args = [
        f"-Djava.class.path={os.pathsep.join(classpath)}",
        f"-Djava.library.path={natives}/java",
        f"-Djna.tmpdir={natives}/jna",
        f"-Dorg.lwjgl.system.SharedLibraryExtractPath={natives}/lwjgl",
        f"-Dio.netty.native.workdir={natives}/netty",
        "-Dminecraft.launcher.brand=rozetka2",
        "-Dminecraft.launcher.version=development",
        "--enable-native-access=ALL-UNNAMED",
        "--add-exports=java.base/jdk.internal.misc=ALL-UNNAMED",
        "-XX:StackShadowPages=32",
        "--",
        version["mainClass"],
        "--username",
        "Rozetka2",
        "--version",
        release,
        "--gameDir",
        str(game),
        "--assetsDir",
        str(assets),
        "--assetIndex",
        index["id"],
        "--uuid",
        "00000000000000000000000000000001",
        "--accessToken",
        "0",
        "--versionType",
        "release",
        "--demo",
        "--width",
        "960",
        "--height",
        "540",
    ]
    (WORK / "launch-args.json").write_text(json.dumps(args, indent=2))
    print(
        f'Prepared Minecraft {release}, Java {version["javaVersion"]["majorVersion"]}, {len(classpath)} jars and {len(objects)} assets',
        flush=True,
    )
    return args


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--prepare-only", action="store_true")
    parser.add_argument("--cached", action="store_true")
    parser.add_argument("--interpreter", action="store_true")
    parser.add_argument("--c2", action="store_true")
    parser.add_argument(
        "--translator",
        type=Path,
        default=ROOT / "build/rozetka2",
        help="Translator executable to use",
    )
    parser.add_argument(
        "--exec",
        action="store_true",
        dest="exec_process",
        help="Replace the launcher process for macOS app activation",
    )
    parser.add_argument(
        "--game-dir", type=Path, help="Isolated development game directory"
    )
    options = parser.parse_args()
    args = (
        json.loads((WORK / "launch-args.json").read_text())
        if options.cached
        else prepare()
    )
    game = (options.game_dir or WORK / "game").expanduser().resolve()
    game.mkdir(parents=True, exist_ok=True)
    if options.game_dir:
        args[args.index("--gameDir") + 1] = str(game)
    if not options.prepare_only:
        # The bundled x86 jemalloc assumes 4 KiB OS pages. This Mac uses
        # 16 KiB; LWJGL's supported system allocator avoids that ABI mismatch.
        args = ["-Dorg.lwjgl.system.allocator=system", *args]
        if not options.interpreter:
            args = [
                "-Xmixed",
                *(
                    ["-XX:CICompilerCount=2"]
                    if options.c2
                    else ["-XX:TieredStopAtLevel=1", "-XX:CICompilerCount=1"]
                ),
                "-XX:+UnlockDiagnosticVMOptions",
                "-XX:-ImplicitNullChecks",
                *args,
            ]
        if os.environ.get("R2_TRACE_COMPILATION"):
            args = ["-XX:+PrintCompilation", *args]
        environment = dict(os.environ, R2_JVM_PROBE="1")
        environment.setdefault(
            "R2_GUEST_CXX_ABI", str(WORK / "libcxx/librozetka2-cxxabi.dylib")
        )
        cpp = Path(environment["R2_GUEST_CXX_ABI"]).expanduser().resolve()
        if not cpp.is_file():
            raise SystemExit("Build the guest C++ runtime first: make libcxx")
        environment["R2_GUEST_CXX_ABI"] = str(cpp)
        driver = WORK / "host-driver/libMoltenVK.dylib"
        if driver.is_file():
            environment.setdefault("R2_HOST_VULKAN", str(driver))
        sdl3 = Path("/opt/homebrew/opt/sdl3/lib/libSDL3.dylib")
        if sdl3.is_file():
            environment.setdefault("R2_HOST_SDL3", str(sdl3))
        command = [
            str(options.translator.expanduser().resolve()),
            "launch",
            "--host-libs",
            str(WORK / "java-x86_64"),
            "--",
            *args,
        ]
        if options.exec_process:
            os.chdir(game)
            os.execvpe(command[0], command, environment)
        process = subprocess.Popen(
            command,
            env=environment,
            cwd=game,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
            bufsize=1,
        )
        guest_status = None
        try:
            for line in process.stdout:
                print(line, end="", flush=True)
                match = re.match(r"^rax = (\d+) ", line)
                if match:
                    guest_status = int(match[1])
            status = process.wait()
        except KeyboardInterrupt:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
            raise SystemExit(130)
        # The generic CLI reports guest RAX separately from its own exit code.
        # A Java exception must remain a failed Minecraft launch.
        if status == 0 and guest_status != 0:
            status = 1
        raise SystemExit(status)
