"""Run Minecraft's actual macOS menu setup through x86 JVM/JNA/ObjC bridge."""

from pathlib import Path
import os
import json
import struct
import subprocess
import sys
import zipfile
from java_fixture import JavaClass, class_strings

root = Path(__file__).resolve().parent.parent
binary = Path(sys.argv[1]).resolve()
work = root / "build/minecraft/objc-test"
work.mkdir(parents=True, exist_ok=True)
peer = JavaClass("R2PeerObject", "ca/weblite/objc/NSObject")
peer.method(
    1,
    "<init>",
    "()V",
    2,
    1,
    b"\x2a\x13"
    + peer.string("NSObject")
    + b"\xb7"
    + peer.ref(10, "ca/weblite/objc/NSObject", "<init>", "(Ljava/lang/String;)V")
    + b"\xb1",
)
peer.method(1, "respondsToSelector", "(J)Z", 1, 3, b"\x04\xac")
peer.write(work)
main = JavaClass("R2MacMenuMain", "java/lang/Object")
check = main.ref(10, "R2MacMenuMain", "check", "(Z)V")
code = (
    b"\x1a\x9a\x00\x0b\xbb"
    + struct.pack(">H", main.cls("java/lang/AssertionError"))
    + b"\x59\xb7"
    + main.ref(10, "java/lang/AssertionError", "<init>", "()V")
    + b"\xbf\xb1"
)
main.method(9, "check", "(Z)V", 2, 1, code)
code = (
    b"\x10\x20\xb8"
    + main.ref(10, "org/lwjgl/sdl/SDLInit", "SDL_Init", "(I)Z")
    + b"\xb8"
    + check
)
code += (
    b"\x13"
    + main.string("rozetka2 macOS menu probe")
    + b"\x11\x01\x40\x11\x00\xc8\x14"
    + main.long(8)
)
code += (
    b"\xb8"
    + main.ref(
        10,
        "org/lwjgl/sdl/SDLVideo",
        "SDL_CreateWindow",
        "(Ljava/lang/CharSequence;IIJ)J",
    )
    + b"\x40\x1f\x09\x94\xb8"
    + check
)
code += b"\xb8" + main.ref(
    10, "com/mojang/blaze3d/platform/MacosUtil", "disableCloseWindowMenuItem", "()V"
)
code += (
    b"\xbb"
    + struct.pack(">H", main.cls("R2PeerObject"))
    + b"\x59\xb7"
    + main.ref(10, "R2PeerObject", "<init>", "()V")
    + b"\x4e"
)
code += (
    b"\x2d\xb6"
    + main.ref(10, "ca/weblite/objc/Proxy", "getPeer", "()Lcom/sun/jna/Pointer;")
    + b"\x3a\x04"
)
code += (
    b"\x19\x04\xb8"
    + main.ref(10, "com/sun/jna/Pointer", "nativeValue", "(Lcom/sun/jna/Pointer;)J")
    + b"\xb8"
    + main.ref(
        10,
        "ca/weblite/objc/RuntimeUtils",
        "getJavaPeer",
        "(J)Lca/weblite/objc/Recipient;",
    )
    + b"\x2d\xa5\x00\x07\x03\xa7\x00\x04\x04\xb8"
    + check
)
code += (
    b"\x19\x04\x13"
    + main.string("release")
    + b"\x03\xbd"
    + struct.pack(">H", main.cls("java/lang/Object"))
    + b"\xb8"
    + main.ref(
        10,
        "ca/weblite/objc/RuntimeUtils",
        "msg",
        "(Lcom/sun/jna/Pointer;Ljava/lang/String;[Ljava/lang/Object;)J",
    )
    + b"\x58"
)
code += (
    b"\xb2"
    + main.ref(9, "java/lang/System", "out", "Ljava/io/PrintStream;")
    + b"\x13"
    + main.string(
        "PASS real x86 JVM Objective-C proxy allocation, Java callback, JNI peer identity and deallocation"
    )
    + b"\xb6"
    + main.ref(10, "java/io/PrintStream", "println", "(Ljava/lang/Object;)V")
)
code += (
    b"\xb2"
    + main.ref(9, "java/lang/System", "out", "Ljava/io/PrintStream;")
    + b"\x13"
    + main.string(
        "PASS actual Minecraft macOS menu setup through x86 JVM, JNA and java-objc-bridge"
    )
)
code += b"\xb6" + main.ref(
    10, "java/io/PrintStream", "println", "(Ljava/lang/Object;)V"
)
code += (
    b"\x1f\xb8"
    + main.ref(10, "org/lwjgl/sdl/SDLVideo", "SDL_DestroyWindow", "(J)V")
    + b"\xb8"
    + main.ref(10, "org/lwjgl/sdl/SDLInit", "SDL_Quit", "()V")
    + b"\xb1"
)
main.method(9, "main", "([Ljava/lang/String;)V", 8, 5, code)
main.write(work)
launch = json.loads((root / "build/minecraft/launch-args.json").read_text())
options = launch[: launch.index("--")]
classpath = next(
    x.split("=", 1)[1] for x in options if x.startswith("-Djava.class.path=")
)
api_list = work / "lwjgl-runtime-api.txt"
for jar in classpath.split(os.pathsep):
    if "/org/lwjgl/lwjgl/" not in jar:
        continue
    with zipfile.ZipFile(jar) as archive:
        entry = "org/lwjgl/system/macosx/ObjCRuntime$Functions.class"
        if entry in archive.namelist():
            names = class_strings(archive.read(entry))
            api_list.write_text("\n".join(names) + "\n")
            break
else:
    raise SystemExit("Actual LWJGL Objective-C function table not found")
audit_guest = work / "audit-x86_64"
subprocess.run(
    [
        "clang",
        "-target",
        "x86_64-apple-macos11",
        "-O1",
        "-fno-stack-protector",
        "-Wl,-no_fixup_chains",
        str(root / "tests/objc_guest.m"),
        "-o",
        str(audit_guest),
        "-lobjc",
        "-framework",
        "Foundation",
    ],
    check=True,
)
audit = subprocess.run(
    [
        str(binary),
        "launch",
        "--host-libs",
        str(audit_guest),
        "--",
        "audit",
        str(api_list),
    ],
    env=dict(os.environ, R2_JVM_PROBE="1"),
    capture_output=True,
    text=True,
    timeout=20,
)
if (
    audit.returncode
    or f"PASS resolved {len(names)} actual LWJGL" not in audit.stdout
    or "rax = 0 " not in audit.stdout
):
    raise SystemExit(audit.stdout + audit.stderr)
options = [x for x in options if not x.startswith("-Djava.class.path=")]
command = [
    str(binary),
    "launch",
    "--host-libs",
    str(root / "build/minecraft/java-x86_64"),
    "--",
    "-Xmixed",
    "-XX:TieredStopAtLevel=1",
    "-XX:CICompilerCount=1",
    "-XX:+UnlockDiagnosticVMOptions",
    "-XX:-ImplicitNullChecks",
    "-Dorg.lwjgl.system.allocator=system",
    "-Djava.class.path=" + str(work) + os.pathsep + classpath,
    *options,
    "--",
    "R2MacMenuMain",
]
with (work / "run.log").open("w") as log:
    result = subprocess.run(
        command,
        env=dict(
            os.environ,
            R2_JVM_PROBE="1",
            R2_HOST_SDL3="/opt/homebrew/opt/sdl3/lib/libSDL3.dylib",
            R2_GUEST_CXX_ABI=str(
                root / "build/minecraft/libcxx/librozetka2-cxxabi.dylib"
            ),
            R2_TRACE_LOADER="1",
            R2_TRACE_OBJC="1",
        ),
        stdout=log,
        stderr=subprocess.STDOUT,
        timeout=240,
    )
output = (work / "run.log").read_text()
if (
    result.returncode
    or "PASS actual Minecraft macOS menu setup" not in output
    or "PASS real x86 JVM Objective-C proxy allocation" not in output
    or "rax = 0 " not in output
):
    raise SystemExit(output)
print(
    "PASS actual Minecraft macOS menu setup through the unmodified x86 JVM, JNA and ObjC bridge"
)
