"""Exercise native interfaces in the real guest JVM without Minecraft bootstrap."""

import json
import argparse
import ctypes
import os
from pathlib import Path
import struct
import re
import subprocess

root = Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser()
parser.add_argument("translator", type=Path, nargs="?", default=root / "build/rozetka2")
parser.add_argument("--jna", action="store_true", help="Probe JNA instead of LWJGL")
parser.add_argument(
    "--oshi", action="store_true", help="Probe OSHI CPU and memory queries"
)
parser.add_argument(
    "--files",
    action="store_true",
    help="Probe JDK filesystem roots, space and file type queries",
)
parser.add_argument(
    "--openal",
    action="store_true",
    help="Probe the real x86 OpenAL driver through LWJGL",
)
parser.add_argument(
    "--opengl",
    action="store_true",
    help="Probe real LWJGL OpenGL initialization on macOS",
)
parser.add_argument(
    "--nio",
    action="store_true",
    help="Probe JDK NIO selector creation, wakeup and close",
)
parser.add_argument(
    "--tls",
    action="store_true",
    help="Use a local TLS server with a test-only trust store; requires --nio",
)
args = parser.parse_args()
if sum((args.jna, args.oshi, args.files, args.openal, args.opengl, args.nio)) > 1:
    parser.error("Choose one native Java probe")
if args.tls and not args.nio:
    parser.error("--tls requires --nio")
work = root / (
    "build/minecraft/nio-tls-test"
    if args.tls
    else (
        "build/minecraft/nio-test"
        if args.nio
        else (
            "build/minecraft/opengl-test"
            if args.opengl
            else (
                "build/minecraft/openal-test"
                if args.openal
                else (
                    "build/minecraft/files-test"
                    if args.files
                    else (
                        "build/minecraft/oshi-test"
                        if args.oshi
                        else (
                            "build/minecraft/jna-test"
                            if args.jna
                            else "build/minecraft/lwjgl-test"
                        )
                    )
                )
            )
        )
    )
)
work.mkdir(parents=True, exist_ok=True)
if args.nio:
    import atexit
    import http.server
    import threading

    if args.tls:
        import hashlib
        import ssl

        certificate, key, trust = (
            work / name
            for name in ("localhost.pem", "localhost-key.pem", "localhost-trust.jks")
        )
        with (work / "certificate.log").open("w") as log:
            subprocess.run(
                [
                    "/usr/bin/openssl",
                    "req",
                    "-x509",
                    "-newkey",
                    "rsa:2048",
                    "-sha256",
                    "-nodes",
                    "-days",
                    "1",
                    "-subj",
                    "/CN=rozetka2-loopback",
                    "-addext",
                    "subjectAltName=IP:127.0.0.1",
                    "-keyout",
                    str(key),
                    "-out",
                    str(certificate),
                ],
                stdout=log,
                stderr=subprocess.STDOUT,
                check=True,
                timeout=30,
            )

        # A single trusted-certificate JKS v2 entry, following the installed
        # OpenJDK JavaKeyStore.engineStore/getPreKeyedHash format. This avoids
        # keytool's unrelated scan of all macOS keystore providers.
        def store_utf(value):
            raw = value.encode("ascii")
            return struct.pack(">H", len(raw)) + raw

        der = ssl.PEM_cert_to_DER_cert(certificate.read_text())
        store = (
            struct.pack(">IIII", 0xFEEDFEED, 2, 1, 2)
            + store_utf("rozetka2-loopback")
            + struct.pack(">q", 0)
        )
        store += store_utf("X.509") + struct.pack(">I", len(der)) + der
        checksum = hashlib.sha1(
            "rozetka2-test".encode("utf-16be") + b"Mighty Aphrodite" + store
        ).digest()
        trust.write_bytes(store + checksum)

    class ProbeHandler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def do_GET(self):
            body = b"rozetka2-nio-ok"
            self.send_response(200)
            self.send_header("Content-Type", "text/plain; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, *unused):
            pass

    http_probe = http.server.ThreadingHTTPServer(("127.0.0.1", 0), ProbeHandler)
    if args.tls:
        server_context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        server_context.load_cert_chain(certificate, key)
        http_probe.socket = server_context.wrap_socket(
            http_probe.socket, server_side=True
        )
    threading.Thread(
        target=http_probe.serve_forever, kwargs={"poll_interval": 0.05}, daemon=True
    ).start()
    atexit.register(http_probe.server_close)
    atexit.register(http_probe.shutdown)
    http_url = f'{"https" if args.tls else "http"}://127.0.0.1:{http_probe.server_port}/nio-probe'
options = []
libraries = []
if not (args.files or args.nio):
    launch = json.loads((root / "build/minecraft/launch-args.json").read_text())
    options = launch[: launch.index("--")]
    classpath = next(
        x.split("=", 1)[1] for x in options if x.startswith("-Djava.class.path=")
    )
    pattern = (
        r"/(net/java/dev/jna|com/github/oshi/oshi-core|org/slf4j/slf4j-api)/"
        if args.oshi
        else (
            r"/net/java/dev/jna/"
            if args.jna
            else (
                r"/org/lwjgl/(lwjgl|lwjgl-opengl)/"
                if args.opengl
                else (
                    r"/org/lwjgl/(lwjgl|lwjgl-openal)/"
                    if args.openal
                    else r"/org/lwjgl/(lwjgl|lwjgl-sdl|lwjgl-vulkan)/"
                )
            )
        )
    )
    libraries = [x for x in classpath.split(os.pathsep) if re.search(pattern, x)]
    if not libraries:
        raise SystemExit(
            "Prepare official libraries with scripts/run_minecraft.py --prepare-only"
        )
    options = [x for x in options if not x.startswith("-Djava.class.path=")]
options.append("-Djava.class.path=" + os.pathsep.join([str(work), *libraries]))
if args.tls:
    options.extend(
        [
            "-Djavax.net.ssl.trustStore=" + str(trust),
            "-Djavax.net.ssl.trustStorePassword=rozetka2-test",
            "-Djavax.net.ssl.trustStoreType=JKS",
        ]
    )

# Class.forName initializes each public LWJGL class, including its JNI and
# native procedure table. Build a Java 5 class directly; no native JVM/javac.
pool = []


def entry(data):
    pool.append(data)
    return len(pool)


def utf(text):
    data = text.encode()
    return entry(b"\1" + struct.pack(">H", len(data)) + data)


def cls(name):
    return entry(b"\7" + struct.pack(">H", utf(name)))


def reference(tag, owner, name, descriptor):
    n, d = utf(name), utf(descriptor)
    signature = entry(b"\x0c" + struct.pack(">HH", n, d))
    return entry(bytes([tag]) + struct.pack(">HH", owner, signature))


this, base = cls("R2LWJGL"), cls("java/lang/Object")
system, printer, reflection = (
    cls("java/lang/System"),
    cls("java/io/PrintStream"),
    cls("java/lang/Class"),
)
out = reference(9, system, "out", "Ljava/io/PrintStream;")
println = reference(10, printer, "println", "(Ljava/lang/Object;)V")
println_int = reference(10, printer, "println", "(I)V")
initialize = reference(
    10, reflection, "forName", "(Ljava/lang/String;)Ljava/lang/Class;"
)
classes = (
    ["java.nio.channels.Selector", "java.net.http.HttpClient"]
    if args.nio
    else (
        ["java.io.File"]
        if args.files
        else (
            ["oshi.SystemInfo"]
            if args.oshi
            else (
                ["com.sun.jna.Native"]
                if args.jna
                else (
                    ["org.lwjgl.system.MemoryUtil", "org.lwjgl.opengl.GL"]
                    if args.opengl
                    else (
                        ["org.lwjgl.system.MemoryUtil", "org.lwjgl.openal.ALC"]
                        if args.openal
                        else [
                            "org.lwjgl.system.MemoryUtil",
                            "org.lwjgl.vulkan.VK",
                            "org.lwjgl.sdl.SDL",
                        ]
                    )
                )
            )
        )
    )
)
code = b""
for name in classes:
    constant = entry(b"\10" + struct.pack(">H", utf(name)))
    code += b"\xb2" + struct.pack(">H", out) + b"\x13" + struct.pack(">H", constant)
    code += (
        b"\xb8" + struct.pack(">H", initialize) + b"\xb6" + struct.pack(">H", println)
    )
integer_calls = (
    []
    if args.jna or args.oshi or args.files or args.openal or args.opengl or args.nio
    else [
        ("org/lwjgl/vulkan/VK", "getInstanceVersionSupported"),
        ("org/lwjgl/sdl/SDLVersion", "SDL_GetVersion"),
    ]
)
for owner, method_name in integer_calls:
    function = reference(10, cls(owner), method_name, "()I")
    code += b"\xb2" + struct.pack(">H", out) + b"\xb8" + struct.pack(">H", function)
    code += b"\xb6" + struct.pack(">H", println_int)
if args.openal:
    get_error = reference(10, cls("org/lwjgl/openal/ALC10"), "alcGetError", "(J)I")
    code += (
        b"\xb2"
        + struct.pack(">H", out)
        + b"\x09\xb8"
        + struct.pack(">H", get_error)
        + b"\xb6"
        + struct.pack(">H", println_int)
    )
if args.nio:
    selector = cls("java/nio/channels/Selector")
    open_selector = reference(10, selector, "open", "()Ljava/nio/channels/Selector;")
    select_now = reference(10, selector, "selectNow", "()I")
    select_wait = reference(10, selector, "select", "(J)I")
    wakeup = reference(10, selector, "wakeup", "()Ljava/nio/channels/Selector;")
    close = reference(10, selector, "close", "()V")
    nano_time = reference(10, system, "nanoTime", "()J")
    print_long = reference(10, printer, "println", "(J)V")
    timeout = entry(b"\5" + struct.pack(">q", 10000))
    entry(b"")
    code += b"\xb8" + struct.pack(">H", open_selector) + b"\x4c"
    code += (
        b"\xb2"
        + struct.pack(">H", out)
        + b"\x2b\xb6"
        + struct.pack(">H", select_now)
        + b"\xb6"
        + struct.pack(">H", println_int)
    )
    code += (
        b"\x2b\xb6"
        + struct.pack(">H", wakeup)
        + b"\x57\xb8"
        + struct.pack(">H", nano_time)
        + b"\x41"
    )
    code += (
        b"\xb2"
        + struct.pack(">H", out)
        + b"\x2b\x14"
        + struct.pack(">H", timeout)
        + b"\xb6"
        + struct.pack(">H", select_wait)
        + b"\xb6"
        + struct.pack(">H", println_int)
    )
    code += (
        b"\xb2"
        + struct.pack(">H", out)
        + b"\xb8"
        + struct.pack(">H", nano_time)
        + b"\x20\x65\xb6"
        + struct.pack(">H", print_long)
    )
    code += b"\x2b\xb6" + struct.pack(">H", close)
    client = cls("java/net/http/HttpClient")
    request = cls("java/net/http/HttpRequest")
    response = cls("java/net/http/HttpResponse")
    new_client = reference(10, client, "newHttpClient", "()Ljava/net/http/HttpClient;")
    create_uri = reference(
        10, cls("java/net/URI"), "create", "(Ljava/lang/String;)Ljava/net/URI;"
    )
    builder = reference(
        10, request, "newBuilder", "(Ljava/net/URI;)Ljava/net/http/HttpRequest$Builder;"
    )
    build = reference(
        11,
        cls("java/net/http/HttpRequest$Builder"),
        "build",
        "()Ljava/net/http/HttpRequest;",
    )
    string_body = reference(
        10,
        cls("java/net/http/HttpResponse$BodyHandlers"),
        "ofString",
        "()Ljava/net/http/HttpResponse$BodyHandler;",
    )
    send = reference(
        10,
        client,
        "send",
        "(Ljava/net/http/HttpRequest;Ljava/net/http/HttpResponse$BodyHandler;)Ljava/net/http/HttpResponse;",
    )
    status_code = reference(11, response, "statusCode", "()I")
    body = reference(11, response, "body", "()Ljava/lang/Object;")
    close_client = reference(10, client, "close", "()V")
    url = entry(b"\10" + struct.pack(">H", utf(http_url)))
    code += (
        b"\xb8" + struct.pack(">H", new_client) + b"\x4c\x13" + struct.pack(">H", url)
    )
    code += (
        b"\xb8"
        + struct.pack(">H", create_uri)
        + b"\xb8"
        + struct.pack(">H", builder)
        + b"\xb9"
        + struct.pack(">H", build)
        + b"\x01\0\x4d"
    )
    code += (
        b"\x2b\x2c\xb8"
        + struct.pack(">H", string_body)
        + b"\xb6"
        + struct.pack(">H", send)
        + b"\x4d"
    )
    code += (
        b"\xb2"
        + struct.pack(">H", out)
        + b"\x2c\xb9"
        + struct.pack(">H", status_code)
        + b"\x01\0\xb6"
        + struct.pack(">H", println_int)
    )
    code += (
        b"\xb2"
        + struct.pack(">H", out)
        + b"\x2c\xb9"
        + struct.pack(">H", body)
        + b"\x01\0\xb6"
        + struct.pack(">H", println)
    )
    code += b"\x2b\xb6" + struct.pack(">H", close_client)
if args.jna:
    size = reference(9, cls("com/sun/jna/Native"), "POINTER_SIZE", "I")
    code += b"\xb2" + struct.pack(">H", out) + b"\xb2" + struct.pack(">H", size)
    code += b"\xb6" + struct.pack(">H", println_int)
    function = cls("com/sun/jna/Function")
    find = reference(
        10,
        function,
        "getFunction",
        "(Ljava/lang/String;Ljava/lang/String;)Lcom/sun/jna/Function;",
    )
    invoke = reference(10, function, "invokeInt", "([Ljava/lang/Object;)I")
    symbol = entry(b"\10" + struct.pack(">H", utf("getpid")))
    for library in (None, "System"):
        code += b"\xb2" + struct.pack(">H", out)
        if library is None:
            code += b"\x01"
        else:
            constant = entry(b"\10" + struct.pack(">H", utf(library)))
            code += b"\x13" + struct.pack(">H", constant)
        code += b"\x13" + struct.pack(">H", symbol)
        code += (
            b"\xb8" + struct.pack(">H", find) + b"\x03\xbd" + struct.pack(">H", base)
        )
        code += (
            b"\xb6"
            + struct.pack(">H", invoke)
            + b"\xb6"
            + struct.pack(">H", println_int)
        )
if args.oshi:
    info = cls("oshi/SystemInfo")
    hardware = cls("oshi/hardware/HardwareAbstractionLayer")
    constructor = reference(10, info, "<init>", "()V")
    get_hardware = reference(
        10, info, "getHardware", "()Loshi/hardware/HardwareAbstractionLayer;"
    )
    for method_name, target, query, result in [
        ("getProcessor", "CentralProcessor", "getLogicalProcessorCount", "I"),
        ("getMemory", "GlobalMemory", "getTotal", "J"),
        ("getMemory", "GlobalMemory", "getAvailable", "J"),
    ]:
        getter = reference(11, hardware, method_name, f"()Loshi/hardware/{target};")
        value = reference(11, cls("oshi/hardware/" + target), query, "()" + result)
        print_value = reference(10, printer, "println", "(" + result + ")V")
        code += (
            b"\xb2"
            + struct.pack(">H", out)
            + b"\xbb"
            + struct.pack(">H", info)
            + b"\x59\xb7"
            + struct.pack(">H", constructor)
        )
        code += (
            b"\xb6"
            + struct.pack(">H", get_hardware)
            + b"\xb9"
            + struct.pack(">H", getter)
            + b"\x01\0"
        )
        code += (
            b"\xb9"
            + struct.pack(">H", value)
            + b"\x01\0\xb6"
            + struct.pack(">H", print_value)
        )
    get_os = reference(
        10, info, "getOperatingSystem", "()Loshi/software/os/OperatingSystem;"
    )
    get_process = reference(
        11,
        cls("oshi/software/os/OperatingSystem"),
        "getCurrentProcess",
        "()Loshi/software/os/OSProcess;",
    )
    get_path = reference(
        11, cls("oshi/software/os/OSProcess"), "getPath", "()Ljava/lang/String;"
    )
    code += (
        b"\xb2"
        + struct.pack(">H", out)
        + b"\xbb"
        + struct.pack(">H", info)
        + b"\x59\xb7"
        + struct.pack(">H", constructor)
    )
    code += (
        b"\xb6"
        + struct.pack(">H", get_os)
        + b"\xb9"
        + struct.pack(">H", get_process)
        + b"\x01\0"
    )
    code += (
        b"\xb9"
        + struct.pack(">H", get_path)
        + b"\x01\0\xb6"
        + struct.pack(">H", println)
    )
if args.files:
    file_type = cls("java/io/File")
    roots = reference(10, file_type, "listRoots", "()[Ljava/io/File;")
    code += (
        b"\xb2"
        + struct.pack(">H", out)
        + b"\xb8"
        + struct.pack(">H", roots)
        + b"\xbe\xb6"
        + struct.pack(">H", println_int)
    )
    constructor = reference(10, file_type, "<init>", "(Ljava/lang/String;)V")
    path = entry(b"\10" + struct.pack(">H", utf("/")))
    print_long = reference(10, printer, "println", "(J)V")
    for query in ("getTotalSpace", "getFreeSpace", "getUsableSpace"):
        method = reference(10, file_type, query, "()J")
        code += (
            b"\xb2"
            + struct.pack(">H", out)
            + b"\xbb"
            + struct.pack(">H", file_type)
            + b"\x59\x13"
            + struct.pack(">H", path)
        )
        code += (
            b"\xb7"
            + struct.pack(">H", constructor)
            + b"\xb6"
            + struct.pack(">H", method)
            + b"\xb6"
            + struct.pack(">H", print_long)
        )
    to_path = reference(10, file_type, "toPath", "()Ljava/nio/file/Path;")
    probe_type = reference(
        10,
        cls("java/nio/file/Files"),
        "probeContentType",
        "(Ljava/nio/file/Path;)Ljava/lang/String;",
    )
    for filename in ("rozetka2-probe.png", "rozetka2-probe.txt"):
        name = entry(b"\10" + struct.pack(">H", utf(filename)))
        code += (
            b"\xb2"
            + struct.pack(">H", out)
            + b"\xbb"
            + struct.pack(">H", file_type)
            + b"\x59\x13"
            + struct.pack(">H", name)
        )
        code += (
            b"\xb7"
            + struct.pack(">H", constructor)
            + b"\xb6"
            + struct.pack(">H", to_path)
        )
        code += (
            b"\xb8"
            + struct.pack(">H", probe_type)
            + b"\xb6"
            + struct.pack(">H", println)
        )
code += b"\xb1"
method, descriptor, code_name = utf("main"), utf("([Ljava/lang/String;)V"), utf("Code")
attribute = struct.pack(">HHI", 6, 4, len(code)) + code + struct.pack(">HH", 0, 0)
data = (
    bytes.fromhex("cafebabe")
    + struct.pack(">HHH", 0, 49, len(pool) + 1)
    + b"".join(pool)
)
data += struct.pack(">HHHHHH", 0x21, this, base, 0, 0, 1)
data += (
    struct.pack(">HHHHHI", 9, method, descriptor, 1, code_name, len(attribute))
    + attribute
    + b"\0\0"
)
(work / "R2LWJGL.class").write_bytes(data)
environment = dict(
    os.environ,
    R2_JVM_PROBE="1",
    R2_GUEST_CXX_ABI=str(
        root
        / (
            "build/minecraft/libcxx/librozetka2-cxxabi.dylib"
            if args.openal
            else "build/minecraft/cxxabi/librozetka2-cxxabi.dylib"
        )
    ),
    R2_HOST_VULKAN=str(root / "build/minecraft/host-driver/libMoltenVK.dylib"),
    R2_HOST_SDL3="/opt/homebrew/opt/sdl3/lib/libSDL3.dylib",
)
translator = args.translator.resolve()
command = [
    str(translator),
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
    *options,
    "--",
    "R2LWJGL",
]
with (work / "run.log").open("w") as log:
    process = subprocess.Popen(
        command, env=environment, stdout=log, stderr=subprocess.STDOUT, text=True
    )
    try:
        status = process.wait(timeout=180)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()
        raise SystemExit(
            "Native Java probe timed out:\n" + (work / "run.log").read_text()
        )
output = (work / "run.log").read_text()
if (
    status
    or "rax = 0 " not in output
    or any("class " + x not in output for x in classes)
):
    raise SystemExit(output)
if args.nio:
    numbers = [int(x) for x in output.splitlines() if x.isdecimal()]
    if (
        len(numbers) != 4
        or numbers[:2] != [0, 0]
        or not 0 <= numbers[2] < 5_000_000_000
        or numbers[3] != 200
        or "rozetka2-nio-ok" not in output.splitlines()
    ):
        raise SystemExit("JDK selector wakeup or HTTP response mismatch:\n" + output)
    print(
        "PASS real x86 JVM NIO selector wakeup, "
        + ("verified HTTPS" if args.tls else "HTTP")
        + " request/response and clean client shutdown"
    )
    raise SystemExit(0)
if args.opengl:
    print("PASS real x86 JVM and LWJGL OpenGL initialization using native macOS CGL")
    raise SystemExit(0)
if args.openal:
    if "0" not in output.splitlines():
        raise SystemExit("OpenAL returned an unexpected initial error:\n" + output)
    print("PASS real x86 JVM, LWJGL OpenAL initialization and guest alcGetError")
    raise SystemExit(0)
if args.jna:
    if (
        "8" not in output.splitlines()
        or output.splitlines().count(str(process.pid)) != 2
    ):
        raise SystemExit("JNA pointer size or native getpid mismatch:\n" + output)
    print(
        "PASS real x86 JVM and JNA: JNI initialization, 64-bit pointers, native getpid and shutdown"
    )
    raise SystemExit(0)
if args.oshi:
    numbers = [int(x) for x in output.splitlines() if x.isdecimal()]
    guest_path = str(root / "build/minecraft/java-x86_64")
    if (
        len(numbers) != 3
        or not (numbers[0] > 0 and numbers[1] > 0 and 0 <= numbers[2] <= numbers[1])
        or guest_path not in output.splitlines()
    ):
        raise SystemExit("OSHI returned invalid CPU/memory data:\n" + output)
    print(
        "PASS real x86 JVM and OSHI: logical CPUs="
        + str(numbers[0])
        + ", memory bytes="
        + str(numbers[1])
        + ", available="
        + str(numbers[2])
    )
    raise SystemExit(0)
if args.files:
    numbers = [int(x) for x in output.splitlines() if x.isdecimal()]
    stats = os.statvfs("/")
    if (
        len(numbers) != 4
        or numbers[0] <= 0
        or numbers[1] != stats.f_blocks * stats.f_frsize
        or not all(0 <= x <= numbers[1] for x in numbers[2:])
    ):
        raise SystemExit("JDK filesystem query mismatch:\n" + output)
    if not {"image/png", "text/plain"}.issubset(output.splitlines()):
        raise SystemExit("JDK native file type query mismatch:\n" + output)
    print(
        "PASS real x86 JVM filesystem roots, disk capacity and native PNG/text MIME type detection"
    )
    raise SystemExit(0)
driver = ctypes.CDLL(environment["R2_HOST_VULKAN"])
driver.vkEnumerateInstanceVersion.argtypes = [ctypes.POINTER(ctypes.c_uint32)]
driver.vkEnumerateInstanceVersion.restype = ctypes.c_int
vulkan_version = ctypes.c_uint32()
if driver.vkEnumerateInstanceVersion(ctypes.byref(vulkan_version)) != 0:
    raise SystemExit("Native driver version query failed")
sdl = ctypes.CDLL(environment["R2_HOST_SDL3"])
sdl.SDL_GetVersion.argtypes = []
sdl.SDL_GetVersion.restype = ctypes.c_int
if any(
    str(x) not in output.splitlines()
    for x in (vulkan_version.value, sdl.SDL_GetVersion())
):
    raise SystemExit("Guest graphics version query mismatch:\n" + output)
print(
    "PASS real x86 JVM and LWJGL JNI: system allocator, Vulkan/SDL3 initialization and native version calls"
)
