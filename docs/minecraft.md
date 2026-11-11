# Modern Minecraft through the x86_64 JVM

The target is modern Java Edition with the entire x86_64 JVM and its generated
machine code running through rozetka2. An ARM JVM with translated JNI libraries
does not meet this target. **The unmodified x86 JVM executes the client through
resource loading and renders the Mojang loading screen and main menu. Keyboard
input starts demo-world loading. Entry into a world and gameplay are still being
verified.**

The development machine has Minecraft 26.3 / LWJGL 3.4.3 assets and an installed
Oracle JDK 25.0.4 for macOS x64. No account or authentication files are needed
for the current JVM work.

The [official version manifest](https://piston-meta.mojang.com/mc/game/version_manifest_v2.json)
lists **26.3 as the latest stable release** and 26.4-snapshot-7 as the latest
development snapshot (checked for this stage on 2026-11-06). The target remains
the newest stable Java Edition.

## Reproduce the current milestone

```sh
make test
make test-cxxabi
make test-jvm
make test-jimage
```

`test-jvm` builds `ports/jvm_probe.c` as x86_64 and loads the installed, unmodified
x86_64 `libjvm.dylib` through rozetka2. The probe executes the library's Mach-O
initializers, calls `JNI_GetDefaultJavaVMInitArgs` and `JNI_GetCreatedJavaVMs`,
then executes registered C/C++ exit callbacks. It never runs the guest binary
through the operating system. The JNI invocation driver bypasses the Java
launcher's Cocoa wrapper; the JVM itself remains x86_64 guest code.

`test-cxxabi` checks real guest vtables, `dynamic_cast`, `typeid`, multiple and
virtual inheritance, and allocation/deallocation. The guest ABI library uses
the RTTI/type-info/exception-class implementation from LLVM libc++abi 18.1.8,
compiled as x86_64. Seven source/license files are downloaded from the
[pinned LLVM release](https://github.com/llvm/llvm-project/tree/llvmorg-18.1.8/libcxxabi),
verified with SHA-256, and retained under `build/minecraft/cxxabi/source`.
`ports/cxxabi_support.cpp` supplies allocation and failure paths for this
exceptions-disabled subset. C++ exception unwinding is not implemented;
allocation failure and exception-requiring failure paths terminate in guest
code. This is not a complete replacement for libc++.

The JVM tests set `R2_GUEST_CXX_ABI` to this guest dylib. The loader uses its
exports for implemented C++ symbols requested by the JVM's libc++ dependency.
RTTI vtables contain guest function addresses, never ARM virtual methods.

`test-jimage` dynamically loads the installed x86_64 `libjimage.dylib`, opens
the JDK's real `lib/modules`, finds `java.base/java/lang/Object.class`, reads
its bytes through `JIMAGE_GetResource`, and closes the image. The extracted
class file is `build/minecraft/Object.class`; its magic, size and Java 25+
class version are checked. This reads class bytes without creating a VM or
executing Java bytecode. The installed JDK currently produces 2412 bytes,
class version 69, SHA-256
`eb2adadee4f1c3d3a889c18700b370a076dc00c5fe5be4ae58a9932e6cd66da7`.

The JDK is supplied by the user and is not downloaded or redistributed:

```sh
R2_JDK_HOME=/absolute/path/to/x64-jdk/Contents/Home make test-jvm
```

The driver requests JNI version 24, the latest constant in this JDK's `jni.h`.
This is an interface version, not the JVM's Java release number.

Query logs are written to `build/minecraft/query-test.log`. To test actual VM
creation and execution of `java.lang.System.getProperty("java.version")`:

```sh
make test-jvm-create
```

This target now passes with the installed JDK: `JNI_CreateJavaVM` returns
`JNI_OK`, the Java call returns `25.0.4`, and `DestroyJavaVM` completes.
`-Xint` still generates x86 machine code inside HotSpot; that code runs through
rozetka2's interpreter too. The JVM binary is unmodified.

`ports/java_launcher.c` is an x86 JNI launcher for a specified Java main class.
It runs on the host main thread and accepts JVM options before `--`.
`python3 scripts/run_minecraft.py --prepare-only` prepares the newest official
stable client, matching x86 native jars and assets, verifies downloaded hashes,
and reuses Prism's library/assets caches. It never reads account files or
existing worlds. `python3 scripts/run_minecraft.py --cached` launches the client
in demo mode in `build/minecraft/game`. This remains a development launch,
not a passing game test. Startup invokes the real Minecraft main method and
loads its x86 LWJGL JNI libraries. The default launch enables HotSpot C1;
`--c2` enables normal tiered C1/C2 compilation, and `--interpreter` selects
`-Xint`. `make test-jvm-hot` checks the same arithmetic loop in all three modes,
including forced compilation of the method doing the arithmetic. It also
checks Java float/double remainder operations, signed zero and extreme exponent
differences through both x86 compilers.

The graphics bridge uses native SDL3 and MoltenVK as macOS window/GPU backends.
The JVM, generated Java machine code and LWJGL JNI remain x86_64. Preparation
extracts the native GPU driver from the matching official LWJGL archive;
`R2_HOST_VULKAN` and `R2_HOST_SDL3` can select other installed backends.
`make test-graphics` runs an x86 probe that creates a Vulkan instance, enumerates
the physical GPU and creates a hidden SDL3 Vulkan surface. This probe passes;
Minecraft rendering and interaction still need verification. Function addresses
returned by these libraries become typed guest stubs. Unsupported guest
graphics callbacks fail before reaching the native library. Vulkan debug-utils
and debug-report callbacks have typed trampolines, tested by
`make test-vulkan-callbacks`. Instance pNext nodes are copied into ABI-sized
shadows; guest memory is preserved. `make test-vulkan-frame` creates a swapchain,
submits an actual GPU clear/copy, presents it and verifies every readback pixel.
SDL log callbacks
are translated into x86 calls, including nested reentry and foreign native
threads. `make test-java-callbacks` verifies Java execution through the real
x86 JVM and LWJGL closure, on both the caller and a foreign native thread.
Repeated callbacks on one OS thread retain guest stack/TLS; its guest TLV and
pthread-key destructors execute when that thread exits. Native entry contexts
are disabled and drained before guest mappings can be released.
The macOS OpenGL framework also has a typed native bridge generated from the
public SDK headers. `make test-opengl` creates an offscreen CGL context, checks
float/double arguments and buffers, clears an actual GPU framebuffer and
verifies its RGBA readback. It also initializes LWJGL OpenGL through the x86 JVM.
`make test-lwjgl` initializes the real x86 LWJGL memory, Vulkan and SDL3 Java
classes through the guest JVM using only the prepared LWJGL jars. Its diagnostic
log is `build/minecraft/lwjgl-test/run.log`.
The probe also calls Vulkan and SDL version functions through Java and compares
their results with the configured native backends. `make test-jna` checks the
real x86 JNA JNI library, native pointer size and normal JVM shutdown.
`make test-oshi` exercises CPU-count, total-memory and available-memory queries
through the real OSHI/JNA Java libraries, including IOKit and Mach VM statistics.
`make test-java-files` checks filesystem enumeration, disk-space queries and
PNG/text MIME detection through the guest JVM's macOS file type detector.
Disk capacity is compared with the host's filesystem API; file types use
CoreServices uniform type identifiers and CoreFoundation UTF-16 strings.
`make test-java-nio` opens a real JDK selector, checks nonblocking selection,
verifies that a pending wakeup interrupts a ten-second wait, and sends an HTTP
request to a loopback server with the real JDK HTTP client. `make test-java-tls`
repeats this over HTTPS using a generated local certificate and a test-only JKS
trust store. Certificate validation remains enabled. Its Unix socket pair uses
the native `socketpair` bridge, whose output bounds, data
transfer, poll readiness, half-close and failure errno are checked by `make test`.
`make test-openal` initializes the real x86 OpenAL Soft library through the
guest JVM and LWJGL, then calls `alcGetError`. `make test-openal-playback`
uploads actual PCM to the official x86 OpenAL driver, starts a source, verifies
that CoreAudio render callbacks consume samples, and closes the device/context.
The guest mixer remains x86; ARM trampolines enter the interpreter from the
native audio thread. `make test-callbacks` also checks native AudioUnit render
buffers, callback return values, property-listener registration/removal and
callback retirement. AudioUnits are disposed during runtime teardown.

`scripts/generate_platform_abi.py` derives 206 additional scalar signatures from
the installed public macOS SDK. `make test-platform` checks UTF-8/UTF-16 and
multiword CFRange arguments, CoreText glyph shaping, native collection callbacks,
CFNetwork proxy settings, file timestamps, xattrs and real file copying.
Aggregate font ABIs also cover CGRect returns, CFRange returns, affine
transforms, glyph-path callbacks and font-data provider release callbacks.
Providers own a native byte copy so cached OS fonts cannot retain unmapped
guest bytes after shutdown.
`make test-objc-native` checks named Objective-C runtime loading, native method
encodings, floating-point arguments/results, selector canonicalization, x86
class-method dispatch and exception-handler callback identity. Native methods
use the native runtime's type encodings; guest class methods execute their x86
implementation. Guest NSObject subclasses receive native class adapters with
matching ivar offsets; retained native callbacks enter their x86 methods.
The tests cover allocation, native-to-guest invocation, super dispatch and
deallocation, including Minecraft's original WLJavaProxy JNI/Java callback.
Block arguments and unsupported aggregate ABIs are rejected.
`make test-objc` exercises Minecraft's own `MacosUtil` menu initialization through
the unmodified x86 JVM, JNA and bundled java-objc-bridge JNI library.
Before starting the JVM, it resolves all 99 function names from the installed
LWJGL Objective-C function table. Unsupported IMP/block conversions have
diagnostic stubs and fail if invoked; ARM implementations never escape as guest
code pointers. `make test-minecraft-opengl` compiles the original client GUI
shaders with the actual OpenGL driver, binds their uniform blocks and verifies
the rendered triangle's pixel coverage and interpolated colors.
`make test-sort` checks native `qsort`/Darwin `qsort_r` with x86 comparators,
nested sorts, original stack/TLS, signed results and callback failures. These
synchronous callbacks do not consume the pinned contexts reserved for retained
OS callbacks; repeated sorts and glyph-path traversals cannot exhaust that pool.
Foreign callback threads retain their decoded block across invocations; nested
reentry keeps a separate block. Byte/permission checks still detect patched or
withdrawn code. The callback test exercises 1024 invocations on one native
thread with changing x86 code, stable stack/TLS and exactly-once destructors.
`make test-stb` loads the game's official x86 STB library, verifies every RGBA
pixel decoded from the actual stone texture against an independent PNG decoder,
and decodes the game's click sound from Ogg/Vorbis to non-silent PCM.
It also compares 150 resize cases from the original STB JNI library with an ARM
scalar reference built from the exact SHA-256-pinned LWJGL 3.4.3 header. Cases
cover linear/sRGB bytes and linear floats, channel layouts, alpha, strides and
up/downscaling, with guarded input/output buffers. XMM SSSE3, word multiply and
SSE2 saturating/averaging instructions have 102,644 checks against independent
ARM NEON results and edge cases, including every PALIGNR immediate.
`scripts/audit_guest_instructions.py` compares the decoder with native `otool`
instruction boundaries, masking Mach-O data-in-code ranges in an analysis copy.
The six official STB, FreeType, OpenAL, SPIRV-Cross, VMA and shaderc libraries
currently have no remaining decoder gaps except deliberate `UD2` traps.
The audit includes optional paths and is not a claim of runtime coverage;
unmarked embedded data can also make a linear disassembly unreliable.
Guest CF collection callback tables remain rejected; known native system tables
are accepted. The futex-style `__ulock_wait` bridge releases the guest engine,
preserves both errno conventions, and uses bounded kernel waits for shutdown.

The launcher selects LWJGL's supported `org.lwjgl.system.allocator=system`
option. The bundled x86 jemalloc assumes 4 KiB pages and rejects this Mac's
16 KiB pages; the guest still sees the real OS page size.

The client now uses a larger guest C++ runtime, built with `make libcxx`.
`scripts/build_libcxx.sh` verifies pinned LLVM 18.1.8 source archives and CMake
modules, builds libc++ as x86_64, and combines it with guest RTTI, C++17 aligned
allocation and LLVM's guest static-initialization guards. The sources and their
licenses remain under `build/minecraft/libcxx/source`. Exceptions are disabled
in this runtime; C++ exception unwinding remains unsupported.

`make test-freetype` parses a real TrueType font with the official x86 driver
and verifies Latin/Cyrillic/Greek bitmaps, metrics, kerning and shutdown.
`make test-minecraft-shaders` compiles actual client GUI, textured GUI, block
and entity shaders using its x86 Shaderc. `make test-minecraft-graphics` uses
the compiled GUI shaders for a Vulkan draw with uniform buffers, vertex
attributes and dynamic rendering, then checks framebuffer pixels. These are
isolated rendering prerequisites; they do not establish a working game window.

`make test-libcxx test-shaders` verifies RTTI, strings, containers, locale,
formatted/file streams and concurrent static initialization. It also loads the
official x86 Shaderc and SPIRV-Cross libraries, compiles a vertex shader to
SPIR-V and translates the result back to GLSL. These probes pass, including
ASan/UBSan checks of the translator. The JVM Java-loop probe also passes in
interpreter, C1 and C2 modes with this C++ runtime.

`make run-minecraft` builds the guest prerequisites and prepares/launches the
newest official stable client. The Python launcher selects the larger C++
runtime by default; `R2_GUEST_CXX_ABI` can override its path for development.
`make minecraft-app` packages the C1 development launcher; `make minecraft-app-c2`
creates a separate `Minecraft-rozetka2-C2.app` and isolated `game-app-c2` profile
for the optimizing x86 compiler. A fresh C2 profile starts with render distance 2,
simulation distance 5 and a 30 FPS limit. Existing profile settings are retained.
Both bundles have native ARM entry points that exec the preparation script and
translator in the same process, rotate diagnostic logs and retain a dSYM for
debugging after subsequent rebuilds. These are local development bundles,
dependent on this checkout, installed x64 JDK and prepared library/asset caches.


## Runtime changes covered by tests

- Full-width ULEB negative bind deltas are accepted only when the resulting
  offset stays inside the selected segment; pointer writes still check bounds.
- Classic weak bind streams run after ordinary rebases/binds. The first visible
  weak definition wins unless a visible strong definition exists. Export-trie
  flags, nlist `N_WEAK_DEF`, weak lookup ordinals, interior-pointer addends,
  hidden providers and malformed streams are tested. Missing coalescing
  candidates preserve the original binding/rebase instead of zeroing it.
  Binding occurs when an image is fixed up; published bindings remain pinned.
- `xchg`, `cmpxchg`, and `xadd` support byte, word, dword and qword operands,
  high byte registers, and the accumulator encodings. Memory operations use
  sequentially consistent host atomics. Additional `LOCK` arithmetic uses sequentially consistent compare/exchange
  loops. Unaligned atomics fail explicitly.
- Mach-O initializer pointers execute in dependency order before the main
  entry point, with guest argc/argv. Registered `atexit` / `__cxa_atexit`
  callbacks execute in reverse order in the interpreter after normal exit.
  An initializer calling `exit` stops subsequent initializers and `main`;
  direct process-exit syscalls skip exit callbacks.
- `dlopen` maps additional x86_64 dylibs, fixes up new images without rebasing
  existing images again, and executes guest constructors. `dlsym` searches guest
  images and returns ABI-declared stubs for bridged native functions. `dladdr`
  reports guest image paths and bases; symbol detail fields may be null.
- Absolute dynamic-library paths, local/global visibility, promotion to global,
  `RTLD_NOLOAD`, reference counts, missing files/dependencies, and `dlerror` are
  tested. `RTLD_FIRST` restricts lookup to the requested image. Images stay
  mapped until guest process exit, including after `dlclose`; unloading is
  not implemented.
- `dlopen(NULL)` has process-wide symbol lookup and balanced handle references,
  including typed native stubs. Relative filenames search library environment
  paths, the working directory and fallback directories. `@loader_path` and
  `@rpath` resolve using the calling guest image. Missing optional libraries
  report `dlerror`, allowing the application's fallback path to execute.
- Native pthreads enter separate guest CPU states, instruction caches, stacks,
  FP state and TLS. Guest callbacks/destructors are interpreted. Mutex,
  condition and Mach semaphore waits release the interpreter execution mutex.
  Join, thread exit, repeated thread creation and aborting a blocked runtime
  are tested; teardown joins workers before releasing image storage. ASan/UBSan
  checks include the real JVM creation probe and guest thread failures.
- `pthread_once` runs guest initializers on the calling guest stack. Concurrent
  callers wait with the interpreter mutex released. Nested initialization,
  completed controls, reinitialized storage and initializer thread exit are
  covered; guest function addresses are never passed to native pthread_once.
- Mach thread/host ports and semaphores are tracked and reclaimed. Clearing
  guest exception ports succeeds without changing the ARM task's exception
  ports; registering a guest Mach exception server remains unsupported.
- Guest VM reservations, fixed commits within owned reservations, partial
  unmap/protection changes and executable x86 memory are tracked. Native ARM
  executable permission is never enabled for guest code. Cached instructions
  are checked against changed bytes and current executable permissions.
- Mach-O TLS templates include initialized data, zero fill, rebased pointers
  and alignment; descriptors resolve storage per native guest thread.
- CPU identification exposes a conservative SSE2 profile. Flags, fences,
  prefetch hints, TSC, MXCSR access and the SIMD instructions used during JVM
  startup are implemented. Control modes beyond masked nearest rounding, FTZ and DAZ are
  still rejected.
- Stack pops to memory calculate the destination after advancing RSP;
  `pop rsp` retains the popped value. `STOSB/W/D/Q`, repeated stores and
  `CLD`/`STD` support forward/backward traversal, zero counts and resumable
  page faults partway through a repeated operation.
- Selected Foundation selectors use typed native calls, including the x86
  structure return for `operatingSystemVersion`; selectors are registered from
  guest strings. Guest C++ vtables and callback addresses never execute as ARM.
- zlib streams keep native allocation callbacks in private shadow structures.
  Guest-visible streams retain guest callback fields; unsupported custom
  allocation callbacks fail. Compression, decompression and reset are tested.
- Guest cwd, soft file limit, signal masks and FP environment are restored.
- Native `libSystem`, CoreFoundation, IOKit and Objective-C runtime scopes return typed guest
  function stubs. IOKit send rights are tracked and reclaimed. Guest process
  creation currently returns POSIX `ENOSYS`; it never submits the x86 JDK's
  helper to the native ARM loader. Java can report this as `IOException`.
- `pthread_kill` and process signals queue virtual x86 signals. Guest masks,
  pending queries, inherited thread masks, temporary `sigsuspend` masks and
  `sigsetjmp`/`siglongjmp` operate on guest state, preserving native host signal
  handling. `make test-thread-signals` also checks jumping out of a signal
  handler and subsequent signal delivery without stale frames.
- Guest memory faults enter virtual SIGSEGV handlers with Darwin x86_64
  `siginfo_t`, `ucontext_t` and machine state. Handler edits to registers,
  XMM state and RIP are restored on return. JVM safepoint polling faults have
  been observed and handled during client startup. Native ARM signal handlers
  remain installed for the host runtime. Alternate signal stacks and complete
  SIGILL/SIGFPE delivery remain unfinished.
- Darwin's x86 `fenv_t` layout is translated explicitly. Masked exceptions and
  default rounding, FTZ and DAZ are supported; other control modes fail. x87 loads/stores,
  stack exchange, `FLDZ`, `FCOMI`/`FUCOMI` (including pop forms),
  `FPREM`/`FPREM1` and nonempty FX/signal contexts use software
  80-bit significands, including partial reductions and quotient condition bits.
  FTZ and DAZ are implemented independently for SSE arithmetic, comparisons
  and conversions, including signed zero. Other x87 arithmetic, MXCSR rounding
  modes, unmasked traps and complete floating-point exceptions
  remain unfinished. The host FP environment is restored after the guest.

`R2_JVM_PROBE=1` enables a diagnostic import profile for this installed JDK.
Known imports whose ABI is not implemented bind to **failure stubs**. Calling
one stops execution with its name. Unknown imports still fail during binding.
This lets initialization be investigated without pretending that unimplemented
functions succeed. It also enables the narrowly declared C++ support used by
the query probe; it does not provide a general C++ runtime.

## Remaining work

The client is being advanced against the installed x86 JVM and official 26.3
jars. Broader native/JNI imports and the LWJGL/SDL/Vulkan rendering, callback,
input and audio paths still need runtime coverage. Translation performance
also needs to be measured during actual gameplay.

A passing JVM probe is a prerequisite, not evidence of a working Minecraft
window or world. Game completion requires an actual rendered, interactive run.
