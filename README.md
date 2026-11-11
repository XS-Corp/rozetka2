# rozetka2

P.S. i know that now it doesnt start, but ill fix it as soon as i can, sry for this

`rozetka2` is an experimental x86_64-to-arm64 translator/runtime for Apple
Silicon Macs. It does not call, require, wrap, or depend on Apple's Rosetta 2.

It now runs a reproducible **GNU nano 9.3 tiny build with UTF-8**: the editor's
x86_64 Mach-O code executes in the interpreter, while declared libc and ncurses
calls cross an explicit bridge to the corresponding arm64 system libraries.
Opening, typing Russian/ASCII text, saving, exiting, reopening, and editing the
saved file are tested in a real pseudo-terminal.

It also runs the **DoomGeneric x86_64 engine with Freedoom 0.13.0** in an
interactive SDL window. Game logic and software rendering execute in the
interpreter; declared SDL2 calls use the installed arm64 library for windowing,
input, presenting the guest framebuffer, and queued PCM output. DMX sound
effects are mixed in guest code; MIDI/MUS music uses macOS's AudioToolbox
synthesizer through declared C APIs. Movement, turning, and shooting
are checked through real SDL keyboard events and rendered frame captures.

It also runs **TyrQuake 0.71 with Quake 1.06 shareware episode data**. The
x86_64 engine, QuakeC VM, software 3D renderer, and sound mixer execute in the
interpreter. SDL presents the framebuffer, receives input, and consumes PCM.
The `e1m1` gameplay test verifies movement, turning, shooting, rendered frames,
and actual audio queue consumption.

This is a supported application milestone, not general macOS compatibility.
The JIT remains a smaller register-only backend; nano currently uses the
interpreter and its on-demand instruction cache.

Work on **modern Minecraft Java Edition through an x86_64 JVM** has started.
The unmodified JDK 25 x64 JVM now creates successfully, executes Java code
(`System.getProperty("java.version")` returns `25.0.4`), and shuts down. Its
generated x86 interpreter code, C1/C2 compiled code, native guest threads and TLS run through
rozetka2. The guest C++ runtime also runs the official x86 Shaderc and SPIRV-Cross
libraries in shader compilation probes. Minecraft 26.3 now renders its loading
screen and main menu; entering a world and gameplay are being verified. See the
[Minecraft development status](docs/minecraft.md) for reproducible probes,
the current blocker, and the remaining runtime work.

## Build and run Doom

In addition to the requirements below, install SDL2 at Homebrew's standard
Apple Silicon path (`brew install sdl2`), and have `unzip` and Python 3 available.

```sh
make doom
make run-doom
# Optional local app bundle that can be opened from Finder:
make doom-app
```

The bundle is `build/Doom-rozetka2.app`. Controls: arrows to move/turn,
Ctrl to shoot, Space to use/open, Shift to run, Esc for the game menu.
Close the window to exit. This milestone uses 320×200 software rendering,
scaled to a 960×600 window. Effects and level music are enabled by default.
F4 opens sound volume controls. The engine also accepts `-nosound`, `-nosfx`,
and `-nomusic` after `--` when launching directly.

The effects backend mixes up to 16 channels at 22050 Hz with stereo panning,
linear resampling, and bounded output buffering. It uses
[SDL queued playback](https://wiki.libsdl.org/SDL2/SDL_QueueAudio), so native
audio threads never call guest function pointers. Music uses the system MIDI
instrument bank; it does not emulate the original AdLib/OPL sound. No SDL_mixer
or external soundfont installation is needed.

The build script downloads and checks SHA-256 for the pinned
[DoomGeneric source](https://github.com/ozkl/doomgeneric) commit
`e257a09527062bfb986b6e5e0780839b6cbd5b71` and
[Freedoom 0.13.0](https://github.com/freedoom/freedoom/releases/tag/v0.13.0).
`ports/doom_sdl.c` and `ports/doom_audio.c` provide the platform adapters and
are compiled as x86_64. The upstream source tree stays unchanged; the build
generates a copy of `i_sound.c` with its unused SDL_mixer header include removed
and enables `FEATURE_SOUND`. Free game data, source,
licenses, logs, and binaries stay under `build/doom/`. No commercial IWAD or
Rosetta is required. The optional bundle is for local development, depends on
the installed SDL2, and keeps license files alongside the game.

## Build and run Quake

The requirements are the same as Doom, plus macOS's `bsdtar` for extracting
the original shareware archive.

```sh
make quake
make run-quake
make quake-app
make test-quake
```

The local app is `build/Quake-rozetka2.app`. It opens The Slipgate Complex
(`e1m1`) in a fixed 320×200 software-rendered window scaled to 960×600.
The startup command uses `viewsize 90` so the HUD leaves the weapon visible
at this resolution. Arrow
keys move/turn, Ctrl fires, Space jumps, and Esc opens the menu. Click in the
game to capture the mouse for aiming; Esc releases it. Original game bindings
can be changed in the Controls menu. Sound effects are enabled. The original
CD soundtrack is not included in the shareware data. Startup palette generation
takes about half a minute on the tested machine.

`scripts/build_quake.sh` pins the
[TyrQuake source](https://github.com/sezero/tyrquake) commit
`653157915975b196e36980a1ef7146485509b69a` and downloads the original
[`quake106.zip` shareware archive](https://ftp.gwdg.de/pub/misc/ftp.idsoftware.com/idstuff/quake/).
It checks SHA-256 of both archives and `pak0.pak`, extracts the DOS/LHa archive
as data, and cross-compiles without running any x86 binary through the OS.
Sources, original archives, licenses, and output stay under `build/quake/`.
The local app bundle keeps the original notices and is a development artifact,
not a standalone redistribution package.

`ports/quake_sdl.c` and `ports/quake_audio.c` replace the platform drivers.
Generated copies of the Unix system/input drivers isolate configuration under
`R2_QUAKE_USERDIR` (default `./userdata`) and initialize the input cvar. The
upstream source tree stays unchanged. The build disables x86 assembly and
uses `-mlong-double-64`, so the engine's wider intermediates use SSE double
precision rather than x87. General x87/80-bit support remains unimplemented.
This milestone uses local single-player networking only (`-noudp`) and the
interpreter; it does not use OpenGL or Rosetta.

`make test-quake` starts a real level, waits for sign-on, feeds SDL keys,
compares player coordinates/yaw/ammunition, validates changing framebuffers,
and checks nonzero queued PCM and successful exit. PNG captures, WAV audio,
and logs are saved in `build/quake-test/`.

## Build and run nano

Requirements: Apple Silicon macOS, Xcode Command Line Tools (`clang`, the macOS
SDK, system libffi), `make`, `curl`, `tar`, `shasum`, and Python 3 for PTY tests.

```sh
make
make nano
./build/rozetka2 launch --host-libs ./build/nano/nano-9.3/src/nano -- notes.txt
```

Use nano's normal keys: **Ctrl+O**, **Enter** to save; **Ctrl+X** to exit.
The terminal mode is restored when the runtime exits, including on an error.

`make nano` downloads the pinned official GNU nano 9.3 release, verifies its
SHA-256, and cross-compiles it without executing x86_64 configure probes. Source,
license, logs, and the resulting executable stay under `build/nano/`. It uses
`--enable-tiny --enable-utf8 --disable-nls`; optional full-build features such as
syntax highlighting and nanorc customization are outside this milestone.
The source and license are available at
[GNU nano downloads](https://www.nano-editor.org/dist/v9/).

The SDK's wide-character ncurses exports reside in `libncurses.5.4`; the build
script creates a local `libncursesw.tbd` linker alias under `build/nano/`.
It does not modify the system SDK or install libraries globally.

## Verify

```sh
make test
make test-nano
make test-doom
make test-doom-audio
make test-quake
```

`make test` covers the decoder, interpreter/JIT comparisons, Mach-O fixtures,
flags and partial registers, integer operations, SSE data movement, on-demand
cache growth, host variadic calls, file permissions, signal callbacks, SSE
floating point/lane semantics, and interpreted `setjmp`/`longjmp` transfers.

`make test-nano` drives two interactive PTY sessions. It creates a file, enters
UTF-8 text and a second line, saves with Ctrl+O, verifies bytes and permissions,
exits, reopens and edits the file, saves again, and checks terminal restoration.
The temporary edited file is removed afterward; the terminal transcript is
saved as `build/nano-pty-transcript.log`.

`make test-doom` needs a graphical macOS session. It starts a real level,
feeds SDL key-down/up events, checks player coordinates, angle, and ammunition,
verifies framebuffer changes, and requires a successful guest exit. Logs and
PNG captures are saved in `build/doom-test/`. The test waits for game tics,
so loading animations do not count as evidence of working controls. It also
checks real SDL device queue consumption, the pistol sound's nonzero PCM,
and the native music graph/clock. `build/doom-test/effects.wav` captures the
same effects PCM sent to SDL (music is synthesized separately).

`make test-doom-audio` additionally runs with `-nomusic`, `-nosfx`, and `-nosound`,
checks a short MUS-format score, and verifies that callback/capture requests
are rejected before crossing into SDL.

When testing an AddressSanitizer build with Homebrew's `sdl2-compat`, its
dynamic SDL3 lookup may require `DYLD_LIBRARY_PATH=/opt/homebrew/opt/sdl3/lib`
on the test command. The normal build does not need this setting.

## Runtime modes

```sh
./build/rozetka2 inspect /path/to/x86_64-mach-o
./build/rozetka2 launch ./freestanding-x86_64 -- arg1 arg2
./build/rozetka2 launch --host-libs ./pie-x86_64 -- arg1 arg2
```

`launch` also resolves `.app` bundles to their executable. Supported Mach-O
loading includes x86_64 universal slices with either FAT byte order, segment
mapping, LC_MAIN, classic rebases/binds, eager lazy binding, export tries,
re-exports, supported chained pointer fixups, `@loader_path`,
`@executable_path`, and `@rpath`. Weak-bind coalescing and full dyld/runtime
initialization remain unsupported.

The default mode uses isolated emulated guest regions and the bounded decoded
block used by the regression fixtures. It provides a minimal argc/argv startup
and selected Darwin syscalls, signals, and cooperative pthread-shaped support.

`--host-libs` is an explicit experimental mode for **trusted guest binaries**.
It uses host-address-backed guest segments, stack, argv, and library objects;
this mode is not a security sandbox. Only declared signatures for libSystem and
system ncurses, SDL2, and selected AudioToolbox/CoreFoundation C APIs are
bridged. Guest
function pointers are interpreted through supported callback paths, never cast
to arm64 executable pointers. Integer and double variadic arguments are converted
between the x86_64 and arm64 ABIs, including supported `va_list` calls.
Positional/long-double printf formats and undeclared library APIs return
unsupported errors.

Declared libm calls also bridge scalar float/double arguments and results.
Guest `setjmp`/`longjmp` contexts save guest registers and return addresses;
they never unwind a native interpreter frame. `qsort` guest callbacks remain
explicitly unsupported.

Host-library mode decodes instructions when reached, caches them by guest
address, and permits programs to run beyond the small-fixture step limit. It
checks instruction fetches against loaded code ranges and guest memory access
against mapped/protected regions. Concurrent host threads, self-modifying code,
arbitrary callbacks, complete signal contexts, Objective-C, general framework APIs, and
full application launching are not implemented.

## Instruction coverage

The interpreter handles instruction forms used by nano, Doom, Quake and the
Minecraft/JVM startup path:

- byte, word, dword, and qword moves; 8/16-to-32/64 `movzx`/`movsx`, and
  32-to-64 `movsxd`; correct legacy high-byte and REX byte-register selection;
- register/memory/immediate integer arithmetic and logic, including `adc`,
  `sbb`, `cmp`, and `test`, with carry, zero, sign, overflow, and parity;
- `inc`/`dec` with preserved carry, `neg`, and `not`;
- conditional branches, conditional moves, and `setcc`;
- shifts/rotates including `shld`/`shrd`, `bswap`, supported multiply/divide forms, sign-extension pairs,
  bit test/set/reset/complement, and `bsf`/`bsr`/`tzcnt`/`lzcnt`;
- stack setup, calls/returns, indirect calls/jumps, and supported `lea` forms;
- string stores and copies (`STOS`/`MOVS`) at all four widths, direction control
  and `REP`, with recoverable faults exposing completed iterations;
- selected SSE register/memory moves, packed bitwise operations, and `pcmpeqd`;
- XMM SSSE3 integer instructions, SSE2 word multiply, signed/unsigned saturating
  byte/word arithmetic and unsigned averages;
- selected scalar/packed SSE float/double arithmetic, square root, min/max,
  comparisons, conversions, shuffles, and explicit rounding. These cover the
  tested default masked floating point environment. General MXCSR status,
  alternate rounding modes, floating point traps, AVX, and the full SIMD
  instruction set are not implemented. Aligned/packed accesses check alignment
  where supported;
- software 80-bit x87 loads/stores and stack state, partial remainders, `FLDZ`,
  and `FCOMI`/`FUCOMI` comparisons including pop forms and masked NaN handling.
  General x87 arithmetic remains unfinished.

This is not a complete x86 ISA implementation. Unsupported forms fail explicitly.
The JIT supports a smaller 32/64-bit register subset: moves and extensions,
arithmetic/logic including carry, signed/equality/unsigned branches, supported
conditional moves and byte `setcc`, `nop`, and `ret`. It has no guest memory
backend or parity condition support. Both raw engines retain their fixture
execution limit.

## Raw function blobs

```sh
printf '\x48\xb8\x2a\x00\x00\x00\x00\x00\x00\x00\xc3' > /tmp/ret42.x64
./build/rozetka2 run --engine interp /tmp/ret42.x64
./build/rozetka2 run --engine jit /tmp/ret42.x64
```

Both return `rax = 42 (0x000000000000002a)`.

The interpreter remains the execution reference for expanding the native JIT.
The nano PTY test is the application-level check that loading, instructions,
ABI conversion, guest callbacks, terminal I/O, and file I/O work together.
