# rozetka2

**PROPRIETARY SOFTWARE - ALL RIGHTS RESERVED. See LICENSE for details.**

`rozetka2` is a clean-room experimental x86_64-to-arm64 translator core for
Apple Silicon Macs. It does not call, require, wrap, or depend on Apple's
Rosetta 2.

This repository is intentionally honest about scope: a full macOS application
translator needs a Mach-O loader, dyld integration, syscall and signal
semantics, thread-local state, memory ordering, floating point, SIMD, exceptions,
self-modifying code handling, debugging/profiling hooks, and a large validated
instruction set. That is a long engineering program, not a single drop-in file.

The current build provides the first independent layer:

- a strict decoder for a small x86_64 subset;
- an interpreter used as the behavioral oracle;
- an arm64 JIT backend that emits executable AArch64 machine code;
- a CLI that runs raw supported x86_64 function blobs;
- Mach-O and `.app` inspection for real launch targets;
- a simple Mach-O launcher for freestanding x86_64 executables without dylib
  dependencies;
- tests that compare interpreter and JIT output where both engines support the
  instruction subset.

## Build

```sh
make
```

## Test

```sh
make test
```

## Run a supported x86_64 function blob

The raw input must be an x86_64 code blob containing a top-level `ret`. The
return value is read from `rax`.

```sh
printf '\x48\xb8\x2a\x00\x00\x00\x00\x00\x00\x00\xc3' > /tmp/ret42.x64
./build/rozetka2 run --engine jit /tmp/ret42.x64
./build/rozetka2 run --engine interp /tmp/ret42.x64
```

Expected result:

```text
rax = 42 (0x000000000000002a)
```

## Inspect a real Mach-O or app bundle

```sh
./build/rozetka2 inspect /bin/ls
./build/rozetka2 launch /System/Applications/Siri.app
./build/rozetka2 launch ./tiny-x86_64 -- arg1 arg2
```

`launch` resolves `.app` bundles to `Contents/MacOS/<executable>` and inspects
the Mach-O slices. If the x86_64 executable is freestanding, has `LC_MAIN`, has a
`__TEXT,__text` section, and has no dylib load commands, `launch` executes its
entry code through the interpreter and prints `rax`. The launcher maps Mach-O
segments into an emulated address space, so supported RIP-relative reads from
mapped sections such as `__TEXT,__cstring` work. Minimal macOS x86_64 `read`,
`write`, `open`, `close`, `getpid`, `kill`, `sigaction`, `sigprocmask`,
`_psynch_mutexwait`, `_psynch_mutexdrop`, `_psynch_cvbroad`,
`_psynch_cvsignal`, `_psynch_cvwait`, `fstat`, `mmap`, `bsdthread_create`,
`bsdthread_terminate`, `thread_selfid`, and `exit` syscalls are handled by the
interpreter. The x86_64 machine-dependent `thread_set_tsd_base` trap is also
handled for freestanding pthread-shaped runtimes.

Guest programs receive a minimal SysV-style startup register state:

- `rdi = argc`
- `rsi = argv`
- `rdx = 0`

`argv[0]` is the launched executable path, and arguments after `--` are appended
as `argv[1...]`.

Ordinary macOS applications usually depend on system dylibs and frameworks. For
those, `launch` stops with a dyld-specific unsupported error instead of
pretending the app ran.

## Supported x86_64 subset

The JIT backend supports register-only straight-line code:

- `mov r64, imm64`
- `mov r64, imm32`
- `mov r64, r64`
- `add r64, r64`
- `add r64, imm8/imm32`
- `sub r64, r64`
- `sub r64, imm8/imm32`
- `xor r64, r64`
- `nop`
- `ret`

The interpreter additionally supports a small program-shaped subset:

- stack setup through `push r64`, `pop r64`, `leave`, and `ret`
- relative `call`, `jmp`, and selected `jcc` forms
- `cmp`, `test`, and flags needed by those branches
- 32-bit and 64-bit `mov`, `add`, `sub`, `xor`, `cmp`, and `test` register forms
- `mov` loads/stores for stack memory such as `[rbp-8]` and `[rbp-4]`
- GS-relative `mov` loads/stores used by the x86_64 Darwin pthread TSD ABI;
  segment overrides on `lea` retain normal x86 address-only semantics
- `lea` for supported addressing forms
- `syscall` for macOS x86_64 `read`, `write`, `open`, `close`, `getpid`, `kill`,
  `sigaction`, `sigprocmask`, `_psynch_mutexwait`, `_psynch_mutexdrop`,
  `_psynch_cvbroad`, `_psynch_cvsignal`, `_psynch_cvwait`, `fstat`, `mmap`,
  `bsdthread_create`, `bsdthread_terminate`, `thread_selfid`, and `exit`
- the x86_64 machine-dependent `thread_set_tsd_base` trap, plus per-guest-thread
  GS bases, registered `bsdthread_create` TSD-base setup, and synthetic Mach
  thread-port copyout to the registered TSD slot
- guest signal delivery on interpreter safe points for installed handlers,
  including pending masks, `SA_SIGINFO`, `SA_RESETHAND`, and `SA_NODEFER`
- cooperative guest thread creation, termination, self-id, and minimal
  mutex/condition-variable wait, drop, signal, and broadcast primitives for
  freestanding pthread-shaped code

Floating point, SIMD, broader syscalls, `munmap`, shared mapping write-back,
dyld binding, Objective-C runtime integration, host-asynchronous signal
interruption, `sigaltstack`, full `ucontext_t` population, full libpthread/TLS
runtime semantics (including TSD key allocation/destructors and TLS images),
preemptive host-backed threading, and complete Mach-O execution are not
implemented yet.

## Why this shape

The interpreter keeps correctness testable before optimization. The JIT backend
is deliberately small and explicit, so every emitted ARM64 instruction can be
audited. Future work should grow this through validated instruction families and
then attach a Mach-O runtime around the core.
