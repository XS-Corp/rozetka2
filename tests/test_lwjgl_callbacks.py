"""Call a Java SDL logger through the real x86 JVM and LWJGL callback machinery."""

import argparse
import json
import os
from pathlib import Path
import re
import struct
import subprocess

root = Path(__file__).resolve().parent.parent
p = argparse.ArgumentParser()
p.add_argument("translator", type=Path)
p.add_argument("--backend", type=Path)
a = p.parse_args()
work = root / "build/minecraft/java-callback-test"
work.mkdir(parents=True, exist_ok=True)
from java_fixture import JavaClass

callback = JavaClass("R2SDLCallback", "org/lwjgl/sdl/SDL_LogOutputFunction")
callback.method(
    1,
    "<init>",
    "()V",
    1,
    1,
    b"\x2a\xb7"
    + callback.ref(10, "org/lwjgl/sdl/SDL_LogOutputFunction", "<init>", "()V")
    + b"\xb1",
)
out = callback.ref(9, "java/lang/System", "out", "Ljava/io/PrintStream;")
code = (
    b"\xb2"
    + out
    + b"\x1f\xb6"
    + callback.ref(10, "java/io/PrintStream", "println", "(J)V")
)
code += (
    b"\xb2"
    + out
    + b"\x16\x05\xb8"
    + callback.ref(
        10, "org/lwjgl/system/MemoryUtil", "memUTF8", "(J)Ljava/lang/String;"
    )
)
code += (
    b"\xb6"
    + callback.ref(10, "java/io/PrintStream", "println", "(Ljava/lang/Object;)V")
    + b"\xb1"
)
callback.method(1, "invoke", "(JIIJ)V", 3, 7, code)
callback.write(work)
main = JavaClass("R2SDLCallbackMain", "java/lang/Object")
set_callback = main.ref(
    10,
    "org/lwjgl/sdl/SDLLog",
    "SDL_SetLogOutputFunction",
    "(Lorg/lwjgl/sdl/SDL_LogOutputFunctionI;J)V",
)
code = (
    b"\xbb"
    + struct.pack(">H", main.cls("R2SDLCallback"))
    + b"\x59\xb7"
    + main.ref(10, "R2SDLCallback", "<init>", "()V")
    + b"\x4c"
)
code += b"\x2b\x14" + main.long(42) + b"\xb8" + set_callback
code += (
    b"\x13"
    + main.string("rozetka2-java-callback")
    + b"\xb8"
    + main.ref(10, "org/lwjgl/sdl/SDLLog", "SDL_Log", "(Ljava/lang/CharSequence;)V")
)
code += (
    b"\x01\x09\xb8"
    + set_callback
    + b"\x2b\xb6"
    + main.ref(10, "org/lwjgl/system/Callback", "free", "()V")
    + b"\xb1"
)
main.method(9, "main", "([Ljava/lang/String;)V", 3, 2, code)
main.write(work)
launch = json.loads((root / "build/minecraft/launch-args.json").read_text())
options = launch[: launch.index("--")]
classpath = next(
    x.split("=", 1)[1] for x in options if x.startswith("-Djava.class.path=")
)
jars = [
    x
    for x in classpath.split(os.pathsep)
    if re.search(r"/org/lwjgl/(lwjgl|lwjgl-sdl)/", x)
]
options = [x for x in options if not x.startswith("-Djava.class.path=")]
environment = dict(
    os.environ,
    R2_JVM_PROBE="1",
    R2_GUEST_CXX_ABI=str(root / "build/minecraft/cxxabi/librozetka2-cxxabi.dylib"),
    R2_HOST_SDL3=(
        str(a.backend.resolve())
        if a.backend
        else "/opt/homebrew/opt/sdl3/lib/libSDL3.dylib"
    ),
)
command = [
    str(a.translator.resolve()),
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
    "-Djava.class.path=" + os.pathsep.join([str(work), *jars]),
    *options,
    "--",
    "R2SDLCallbackMain",
]
logfile = work / ("foreign.log" if a.backend else "run.log")
with logfile.open("w") as log:
    result = subprocess.run(
        command, env=environment, stdout=log, stderr=subprocess.STDOUT, timeout=180
    )
output = logfile.read_text()
if (
    result.returncode
    or "rax = 0 " not in output
    or "42" not in output.splitlines()
    or "rozetka2-java-callback" not in output.splitlines()
):
    raise SystemExit(output)
print(
    "PASS real x86 JVM and LWJGL SDL callback: Java invocation, userdata, UTF-8 message and closure release"
)
