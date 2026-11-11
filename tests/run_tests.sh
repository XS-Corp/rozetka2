#!/bin/sh
set -eu

BIN="${1:-./build/rozetka2}"
TMPDIR="${TMPDIR:-/tmp}/rozetka2-tests.$$"
mkdir -p "$TMPDIR"
trap 'rm -rf "$TMPDIR"' EXIT INT TERM

pass=0

run_case() {
    name="$1"
    expected="$2"
    bytes="$3"
    file="$TMPDIR/$name.bin"

    printf "$bytes" > "$file"

    interp="$("$BIN" run --engine interp "$file")"
    jit="$("$BIN" run --engine jit "$file")"

    expected_line="rax = $expected"
    case "$interp" in
        "$expected_line"*) ;;
        *)
            echo "FAIL $name interp: expected '$expected_line', got '$interp'" >&2
            exit 1
            ;;
    esac

    case "$jit" in
        "$expected_line"*) ;;
        *)
            echo "FAIL $name jit: expected '$expected_line', got '$jit'" >&2
            exit 1
            ;;
    esac

    if [ "$interp" != "$jit" ]; then
        echo "FAIL $name: interp/jit mismatch" >&2
        echo "  interp: $interp" >&2
        echo "  jit:    $jit" >&2
        exit 1
    fi

    pass=$((pass + 1))
    echo "PASS $name"
}

run_interp_case() {
    name="$1"
    expected="$2"
    bytes="$3"
    file="$TMPDIR/$name.bin"

    printf "$bytes" > "$file"

    interp="$("$BIN" run --engine interp "$file")"
    expected_line="rax = $expected"
    case "$interp" in
        "$expected_line"*) ;;
        *)
            echo "FAIL $name interp: expected '$expected_line', got '$interp'" >&2
            exit 1
            ;;
    esac

    pass=$((pass + 1))
    echo "PASS $name"
}

run_case ret42 42 '\x48\xb8\x2a\x00\x00\x00\x00\x00\x00\x00\xc3'
run_case add_regs 42 '\x48\xb8\x0a\x00\x00\x00\x00\x00\x00\x00\x48\xb9\x20\x00\x00\x00\x00\x00\x00\x00\x48\x01\xc8\xc3'
run_case sub_regs 42 '\x48\xb8\x64\x00\x00\x00\x00\x00\x00\x00\x48\xb9\x3a\x00\x00\x00\x00\x00\x00\x00\x48\x29\xc8\xc3'
run_case mov_r8_add_imm 47 '\x49\xb8\x2a\x00\x00\x00\x00\x00\x00\x00\x4c\x89\xc0\x48\x83\xc0\x05\xc3'
run_case xor_zero_add 5 '\x48\xb8\xff\xff\xff\xff\xff\xff\xff\xff\x48\x31\xc0\x48\x83\xc0\x05\xc3'
run_case add_negative_imm 40 '\x48\xb8\x2a\x00\x00\x00\x00\x00\x00\x00\x48\x83\xc0\xfe\xc3'
run_case sub_negative_imm 44 '\x48\xb8\x2a\x00\x00\x00\x00\x00\x00\x00\x48\x83\xe8\xfe\xc3'
run_case full_imm64 1311768467463790320 '\x48\xb8\xf0\xde\xbc\x9a\x78\x56\x34\x12\xc3'
run_case imm64_minus_one 18446744073709551615 '\x48\xb8\xff\xff\xff\xff\xff\xff\xff\xff\xc3'
run_case mov_imm32_sign_ext 18446744073709551615 '\x48\xc7\xc0\xff\xff\xff\xff\xc3'
run_case mov_8b_add_03 42 '\x48\xb9\x20\x00\x00\x00\x00\x00\x00\x00\x48\xb8\x0a\x00\x00\x00\x00\x00\x00\x00\x48\x8b\xd0\x48\x03\xca\x48\x89\xc8\xc3'
run_interp_case stack_frame 42 '\x55\x48\x89\xe5\x48\x83\xec\x10\x48\xc7\x45\xf8\x2a\x00\x00\x00\x48\x8b\x45\xf8\xc9\xc3'
run_interp_case call_helper_after_ret 42 '\xe8\x01\x00\x00\x00\xc3\x48\xb8\x2a\x00\x00\x00\x00\x00\x00\x00\xc3'
run_case cmp_jne_loop 6 '\x48\xb8\x00\x00\x00\x00\x00\x00\x00\x00\x48\xb9\x03\x00\x00\x00\x00\x00\x00\x00\x48\x01\xc8\x48\x83\xe9\x01\x48\x83\xf9\x00\x75\xf3\xc3'
run_case unsigned_cmp_jb_loop 3 '\xb8\x00\x00\x00\x00\xb9\x03\x00\x00\x00\x83\xc0\x01\x39\xc8\x72\xf9\xc3'
run_case setcc_al 1 '\xb8\x2a\x00\x00\x00\x83\xf8\x2a\xb8\x00\x00\x00\x00\x0f\x94\xc0\xc3'
run_interp_case setcc_stack_byte 256 '\x48\x83\xec\x10\x48\xc7\x04\x24\x00\x00\x00\x00\x31\xc0\x0f\x94\x44\x24\x01\x48\x8b\x04\x24\x48\x83\xc4\x10\xc3'
run_case adc_carry_chain 42 '\x48\xb8\xff\xff\xff\xff\xff\xff\xff\xff\x48\x83\xc0\x01\x0f\x92\xc2\xb8\x29\x00\x00\x00\x48\x83\xd0\x00\xc3'
run_case sbb_borrow_chain 42 '\x31\xc0\x48\x83\xe8\x01\x0f\x92\xc2\xb8\x2b\x00\x00\x00\x48\x83\xd8\x00\xc3'
run_case movzx_ah 129 '\xb8\x80\x81\x00\x00\x0f\xb6\xc4\xc3'
run_case movsx_al_to_eax 4294967168 '\xb8\x80\x00\x00\x00\x0f\xbe\xc0\xc3'
run_case movsxd_eax 18446744071562067968 '\xb8\x00\x00\x00\x80\x48\x63\xc0\xc3'
run_interp_case movsx_stack_word 4294967295 '\x48\x83\xec\x10\xc7\x04\x24\xff\xff\x00\x00\x0f\xbf\x04\x24\x48\x83\xc4\x10\xc3'
run_interp_case clang_like_i32_main 42 '\x55\x48\x89\xe5\xc7\x45\xfc\x00\x00\x00\x00\xb8\x2a\x00\x00\x00\x5d\xc3'
run_interp_case thread_tsd_base_selector 15 '\x48\x89\xe7\x31\xf6\xb8\x03\x00\x00\x03\x0f\x05\xc3'
run_interp_case thread_tsd_base 42 '\x48\x83\xec\x20\x48\xc7\x04\x24\x15\x00\x00\x00\x48\x89\xe7\x31\xf6\xb8\x03\x00\x00\x03\x0f\x05\x65\x48\x8b\x04\x25\x00\x00\x00\x00\x48\x83\xc0\x15\x65\x48\x89\x04\x25\x08\x00\x00\x00\x65\x48\x8b\x04\x25\x08\x00\x00\x00\x48\x83\xc4\x20\xc3'
run_interp_case gs_rip_relative 42 '\x48\x83\xec\x20\x48\xc7\x04\x24\x2a\x00\x00\x00\x48\x89\xe7\x48\x83\xef\x24\x31\xf6\xb8\x03\x00\x00\x03\x0f\x05\x65\x48\x8b\x05\x00\x00\x00\x00\x48\x83\xc4\x20\xc3'
run_interp_case gs_lea_ignores_segment 42 '\x48\x89\xe1\x65\x48\x8d\x04\x24\x48\x29\xc8\x48\x83\xc0\x2a\xc3'
run_interp_case syscall_exit_no_ret 7 '\xb8\x01\x00\x00\x02\xbf\x07\x00\x00\x00\x0f\x05'
run_interp_case sigaction_set_query 42 '\x48\x83\xec\x30\x48\xc7\x04\x24\x01\x00\x00\x00\x48\xc7\x44\x24\x08\x00\x00\x00\x00\xc7\x44\x24\x10\x00\x00\x00\x00\xc7\x44\x24\x14\x02\x00\x00\x00\x48\xc7\x44\x24\x18\xff\xff\xff\xff\x48\xc7\x44\x24\x20\xff\xff\xff\xff\xb8\x2e\x00\x00\x02\xbf\x0d\x00\x00\x00\x48\x89\xe6\x48\x8d\x54\x24\x18\x0f\x05\x48\x83\xf8\x00\x0f\x85\x87\x00\x00\x00\x48\x8b\x5c\x24\x18\x48\x85\xdb\x0f\x85\x79\x00\x00\x00\x8b\x4c\x24\x20\x85\xc9\x0f\x85\x6d\x00\x00\x00\x8b\x4c\x24\x24\x85\xc9\x0f\x85\x61\x00\x00\x00\x48\xc7\x44\x24\x18\xff\xff\xff\xff\x48\xc7\x44\x24\x20\xff\xff\xff\xff\xb8\x2e\x00\x00\x02\xbf\x0d\x00\x00\x00\x31\xf6\x48\x8d\x54\x24\x18\x0f\x05\x48\x83\xf8\x00\x0f\x85\x32\x00\x00\x00\x48\x8b\x5c\x24\x18\x48\x83\xfb\x01\x0f\x85\x23\x00\x00\x00\x8b\x4c\x24\x20\x85\xc9\x0f\x85\x17\x00\x00\x00\x8b\x4c\x24\x24\x83\xf9\x02\x0f\x85\x0a\x00\x00\x00\x48\x83\xc4\x30\xb8\x2a\x00\x00\x00\xc3\x48\x83\xc4\x30\xb8\x63\x00\x00\x00\xc3'
run_interp_case signal_delivery_handler 42 '\x48\x81\xec\x80\x00\x00\x00\x48\x8d\x05\x5e\x00\x00\x00\x48\x89\x04\x24\x48\xc7\x44\x24\x08\x00\x00\x00\x00\xc7\x44\x24\x10\x00\x00\x00\x00\xc7\x44\x24\x14\x00\x00\x00\x00\xb8\x2e\x00\x00\x02\xbf\x1e\x00\x00\x00\x48\x89\xe6\x31\xd2\x0f\x05\x4c\x8d\x64\x24\x40\x41\xc7\x04\x24\x00\x00\x00\x00\xb8\x14\x00\x00\x02\x0f\x05\x89\xc7\xb8\x25\x00\x00\x02\xbe\x1e\x00\x00\x00\x31\xd2\x0f\x05\x41\x8b\x04\x24\x48\x81\xc4\x80\x00\x00\x00\xc3\x41\xc7\x04\x24\x2a\x00\x00\x00\xc3'
run_interp_case signal_mask_pending_unblock 42 '\x48\x81\xec\x90\x00\x00\x00\x48\x8d\x05\x94\x00\x00\x00\x48\x89\x04\x24\x48\xc7\x44\x24\x08\x00\x00\x00\x00\xc7\x44\x24\x10\x00\x00\x00\x00\xc7\x44\x24\x14\x00\x00\x00\x00\xb8\x2e\x00\x00\x02\xbf\x1e\x00\x00\x00\x48\x89\xe6\x31\xd2\x0f\x05\x4c\x8d\x64\x24\x40\x41\xc7\x04\x24\x00\x00\x00\x00\xc7\x44\x24\x30\x00\x00\x00\x20\xb8\x30\x00\x00\x02\xbf\x01\x00\x00\x00\x48\x8d\x74\x24\x30\x31\xd2\x0f\x05\xb8\x14\x00\x00\x02\x0f\x05\x89\xc7\xb8\x25\x00\x00\x02\xbe\x1e\x00\x00\x00\x31\xd2\x0f\x05\x41\xc7\x04\x24\x07\x00\x00\x00\xb8\x30\x00\x00\x02\xbf\x02\x00\x00\x00\x48\x8d\x74\x24\x30\x31\xd2\x0f\x05\x41\x8b\x04\x24\x48\x81\xc4\x90\x00\x00\x00\xc3\x41\xc7\x04\x24\x2a\x00\x00\x00\xc3'
run_interp_case signal_siginfo_handler 30 '\x48\x81\xec\x90\x00\x00\x00\x48\x8d\x05\x5e\x00\x00\x00\x48\x89\x04\x24\x48\xc7\x44\x24\x08\x00\x00\x00\x00\xc7\x44\x24\x10\x00\x00\x00\x00\xc7\x44\x24\x14\x40\x00\x00\x00\xb8\x2e\x00\x00\x02\xbf\x1e\x00\x00\x00\x48\x89\xe6\x31\xd2\x0f\x05\x4c\x8d\x64\x24\x40\x41\xc7\x04\x24\x00\x00\x00\x00\xb8\x14\x00\x00\x02\x0f\x05\x89\xc7\xb8\x25\x00\x00\x02\xbe\x1e\x00\x00\x00\x31\xd2\x0f\x05\x41\x8b\x04\x24\x48\x81\xc4\x90\x00\x00\x00\xc3\x8b\x06\x41\x89\x04\x24\xc3'
run_interp_case signal_resethand_default 158 '\x48\x81\xec\x90\x00\x00\x00\x48\x8d\x05\x75\x00\x00\x00\x48\x89\x04\x24\x48\xc7\x44\x24\x08\x00\x00\x00\x00\xc7\x44\x24\x10\x00\x00\x00\x00\xc7\x44\x24\x14\x04\x00\x00\x00\xb8\x2e\x00\x00\x02\xbf\x1e\x00\x00\x00\x48\x89\xe6\x31\xd2\x0f\x05\x4c\x8d\x64\x24\x40\x41\xc7\x04\x24\x00\x00\x00\x00\xb8\x14\x00\x00\x02\x0f\x05\x89\xc7\xb8\x25\x00\x00\x02\xbe\x1e\x00\x00\x00\x31\xd2\x0f\x05\xb8\x14\x00\x00\x02\x0f\x05\x89\xc7\xb8\x25\x00\x00\x02\xbe\x1e\x00\x00\x00\x31\xd2\x0f\x05\x41\x8b\x04\x24\x48\x81\xc4\x90\x00\x00\x00\xc3\x41\xc7\x04\x24\x2a\x00\x00\x00\xc3'
run_interp_case signal_nodefer_nested 2 '\x48\x81\xec\x90\x00\x00\x00\x48\x8d\x05\x5e\x00\x00\x00\x48\x89\x04\x24\x48\xc7\x44\x24\x08\x00\x00\x00\x00\xc7\x44\x24\x10\x00\x00\x00\x00\xc7\x44\x24\x14\x10\x00\x00\x00\xb8\x2e\x00\x00\x02\xbf\x1e\x00\x00\x00\x48\x89\xe6\x31\xd2\x0f\x05\x4c\x8d\x64\x24\x40\x41\xc7\x04\x24\x00\x00\x00\x00\xb8\x14\x00\x00\x02\x0f\x05\x89\xc7\xb8\x25\x00\x00\x02\xbe\x1e\x00\x00\x00\x31\xd2\x0f\x05\x41\x8b\x04\x24\x48\x81\xc4\x90\x00\x00\x00\xc3\x41\x8b\x04\x24\x83\xc0\x01\x41\x89\x04\x24\x83\xf8\x02\x0f\x8d\x17\x00\x00\x00\xb8\x14\x00\x00\x02\x0f\x05\x89\xc7\xb8\x25\x00\x00\x02\xbe\x1e\x00\x00\x00\x31\xd2\x0f\x05\xc3'

cc -std=c17 -Wall -Wextra -Werror -O2 tests/make_tiny_macho.c -o "$TMPDIR/make_tiny_macho"
cc -std=c17 -Wall -Wextra -Werror -O2 tests/make_argv_macho.c -o "$TMPDIR/make_argv_macho"
cc -std=c17 -Wall -Wextra -Werror -O2 tests/make_mmap_fstat_macho.c -o "$TMPDIR/make_mmap_fstat_macho"
cc -std=c17 -Wall -Wextra -Werror -O2 tests/make_pthread_macho.c -o "$TMPDIR/make_pthread_macho"
cc -std=c17 -Wall -Wextra -Werror -O2 tests/make_bsdthread_register_macho.c -o "$TMPDIR/make_bsdthread_register_macho"
cc -std=c17 -Wall -Wextra -Werror -O2 tests/make_pthread_tsd_macho.c -o "$TMPDIR/make_pthread_tsd_macho"
cc -std=c17 -Wall -Wextra -Werror -O2 tests/make_dyld_macho.c -o "$TMPDIR/make_dyld_macho"
printf 'file io from rozetka2\n' > "$TMPDIR/input.txt"
"$TMPDIR/make_tiny_macho" "$TMPDIR/tiny_macho" "$TMPDIR/input.txt"
macho_output="$("$BIN" launch "$TMPDIR/tiny_macho")"
case "$macho_output" in
    *"launched simple x86_64 Mach-O via interpreter"*) ;;
    *)
        echo "FAIL tiny_macho_launch: expected launch output with rax = 42" >&2
        echo "$macho_output" >&2
        exit 1
        ;;
esac
case "$macho_output" in
    *"rax = 42"*) ;;
    *)
        echo "FAIL tiny_macho_launch: expected launch output with rax = 42" >&2
        echo "$macho_output" >&2
        exit 1
        ;;
esac
case "$macho_output" in
    *"file io from rozetka2"*) ;;
    *)
        echo "FAIL tiny_macho_launch: expected guest file io output" >&2
        echo "$macho_output" >&2
        exit 1
        ;;
esac
pass=$((pass + 1))
echo "PASS tiny_macho_launch"

"$TMPDIR/make_argv_macho" "$TMPDIR/argv_macho"
argv_output="$("$BIN" launch "$TMPDIR/argv_macho" -- "argv from rozetka2")"
case "$argv_output" in
    *"argv from rozetka2"*) ;;
    *)
        echo "FAIL argv_macho_launch: expected guest argv output" >&2
        echo "$argv_output" >&2
        exit 1
        ;;
esac
case "$argv_output" in
    *"rax = 42"*) ;;
    *)
        echo "FAIL argv_macho_launch: expected rax = 42" >&2
        echo "$argv_output" >&2
        exit 1
        ;;
esac
pass=$((pass + 1))
echo "PASS argv_macho_launch"

printf 'mmap/fstat from rozetka2\n' > "$TMPDIR/mmap_input.txt"
"$TMPDIR/make_mmap_fstat_macho" "$TMPDIR/mmap_fstat_macho" "$TMPDIR/mmap_input.txt"
mmap_output="$("$BIN" launch "$TMPDIR/mmap_fstat_macho")"
case "$mmap_output" in
    *"mmap/fstat from rozetka2"*) ;;
    *)
        echo "FAIL mmap_fstat_macho_launch: expected guest mmap/fstat output" >&2
        echo "$mmap_output" >&2
        exit 1
        ;;
esac
case "$mmap_output" in
    *"rax = 42"*) ;;
    *)
        echo "FAIL mmap_fstat_macho_launch: expected rax = 42" >&2
        echo "$mmap_output" >&2
        exit 1
        ;;
esac
pass=$((pass + 1))
echo "PASS mmap_fstat_macho_launch"

"$TMPDIR/make_pthread_macho" "$TMPDIR/pthread_macho"
pthread_output="$("$BIN" launch "$TMPDIR/pthread_macho")"
case "$pthread_output" in
    *"rax = 42"*) ;;
    *)
        echo "FAIL pthread_macho_launch: expected rax = 42" >&2
        echo "$pthread_output" >&2
        exit 1
        ;;
esac
pass=$((pass + 1))
echo "PASS pthread_macho_launch"

"$TMPDIR/make_bsdthread_register_macho" "$TMPDIR/bsdthread_register_macho"
bsdthread_register_output="$("$BIN" launch "$TMPDIR/bsdthread_register_macho")"
case "$bsdthread_register_output" in
    *"rax = 42"*) ;;
    *)
        echo "FAIL bsdthread_register_macho_launch: expected rax = 42" >&2
        echo "$bsdthread_register_output" >&2
        exit 1
        ;;
esac
pass=$((pass + 1))
echo "PASS bsdthread_register_macho_launch"

"$TMPDIR/make_pthread_tsd_macho" "$TMPDIR/pthread_tsd_macho"
pthread_tsd_output="$("$BIN" launch "$TMPDIR/pthread_tsd_macho")"
case "$pthread_tsd_output" in
    *"pthread tsd isolated"*) ;;
    *)
        echo "FAIL pthread_tsd_macho_launch: expected guest TSD isolation output" >&2
        echo "$pthread_tsd_output" >&2
        exit 1
        ;;
esac
case "$pthread_tsd_output" in
    *"rax = 42"*) ;;
    *)
        echo "FAIL pthread_tsd_macho_launch: expected rax = 42" >&2
        echo "$pthread_tsd_output" >&2
        exit 1
        ;;
esac
pass=$((pass + 1))
echo "PASS pthread_tsd_macho_launch"

mkdir -p "$TMPDIR/dyld_bind"
"$TMPDIR/make_dyld_macho" bind \
    "$TMPDIR/dyld_bind/main" \
    "$TMPDIR/dyld_bind/libanswer.dylib"
dyld_bind_output="$("$BIN" launch "$TMPDIR/dyld_bind/main")"
case "$dyld_bind_output" in
    *"dylib: @loader_path/libanswer.dylib"*) ;;
    *)
        echo "FAIL dylib_loader_path_bind: expected parsed @loader_path dependency" >&2
        echo "$dyld_bind_output" >&2
        exit 1
        ;;
esac
case "$dyld_bind_output" in
    *"rax = 42"*) ;;
    *)
        echo "FAIL dylib_loader_path_bind: expected eager GOT bind call to return 42" >&2
        echo "$dyld_bind_output" >&2
        exit 1
        ;;
esac
pass=$((pass + 1))
echo "PASS dylib_loader_path_bind"

"$TMPDIR/make_dyld_macho" bind-backward \
    "$TMPDIR/dyld_bind/main" "$TMPDIR/dyld_bind/libanswer.dylib"
dyld_bind_output="$("$BIN" launch "$TMPDIR/dyld_bind/main")"
case "$dyld_bind_output" in
    *"rax = 42"*) ;;
    *) echo "FAIL dylib_backward_bind: $dyld_bind_output" >&2; exit 1 ;;
esac
pass=$((pass + 1))
echo "PASS dylib_backward_bind"

"$TMPDIR/make_dyld_macho" bind-backward-invalid \
    "$TMPDIR/dyld_bind/main" "$TMPDIR/dyld_bind/libanswer.dylib"
if "$BIN" launch "$TMPDIR/dyld_bind/main" >"$TMPDIR/invalid-bind.log" 2>&1; then
    echo "FAIL dylib_backward_bind_bounds: accepted out-of-bounds address" >&2
    exit 1
fi
if ! grep -q 'dyld bind address is outside segment' "$TMPDIR/invalid-bind.log"; then
    cat "$TMPDIR/invalid-bind.log" >&2
    exit 1
fi
pass=$((pass + 1))
echo "PASS dylib_backward_bind_bounds"

mkdir -p "$TMPDIR/dyld_rebase"
"$TMPDIR/make_dyld_macho" rebase \
    "$TMPDIR/dyld_rebase/main" \
    "$TMPDIR/dyld_rebase/librebase.dylib"
dyld_rebase_output="$("$BIN" launch "$TMPDIR/dyld_rebase/main")"
case "$dyld_rebase_output" in
    *"rax = 42"*) ;;
    *)
        echo "FAIL dylib_rebase_slide: expected rebased dylib data pointer to yield 42" >&2
        echo "$dyld_rebase_output" >&2
        exit 1
        ;;
esac
pass=$((pass + 1))
echo "PASS dylib_rebase_slide"

mkdir -p "$TMPDIR/dyld_weak"
rm -f "$TMPDIR/dyld_weak/libdefinitely_missing.dylib"
"$TMPDIR/make_dyld_macho" weak "$TMPDIR/dyld_weak/main"
dyld_weak_output="$("$BIN" launch "$TMPDIR/dyld_weak/main")"
case "$dyld_weak_output" in
    *"dylib: @loader_path/libdefinitely_missing.dylib"*) ;;
    *)
        echo "FAIL dylib_weak_missing: expected parsed LC_LOAD_WEAK_DYLIB path" >&2
        echo "$dyld_weak_output" >&2
        exit 1
        ;;
esac
case "$dyld_weak_output" in
    *"rax = 42"*) ;;
    *)
        echo "FAIL dylib_weak_missing: expected missing weak dependency to be ignored" >&2
        echo "$dyld_weak_output" >&2
        exit 1
        ;;
esac
pass=$((pass + 1))
echo "PASS dylib_weak_missing"

mkdir -p \
    "$TMPDIR/dyld_executable_path/app/bin" \
    "$TMPDIR/dyld_executable_path/app/support" \
    "$TMPDIR/dyld_executable_path/frameworks/nested"
"$TMPDIR/make_dyld_macho" executable-path \
    "$TMPDIR/dyld_executable_path/app/bin/main" \
    "$TMPDIR/dyld_executable_path/frameworks/nested/libmiddle.dylib" \
    "$TMPDIR/dyld_executable_path/app/support/libleaf.dylib"
dyld_middle_inspect="$("$BIN" inspect \
    "$TMPDIR/dyld_executable_path/frameworks/nested/libmiddle.dylib")"
case "$dyld_middle_inspect" in
    *"dylib: @executable_path/../support/libleaf.dylib"*) ;;
    *)
        echo "FAIL dylib_executable_path_nested: expected nested @executable_path dependency" >&2
        echo "$dyld_middle_inspect" >&2
        exit 1
        ;;
esac
dyld_executable_path_output="$("$BIN" launch \
    "$TMPDIR/dyld_executable_path/app/bin/main")"
case "$dyld_executable_path_output" in
    *"rax = 42"*) ;;
    *)
        echo "FAIL dylib_executable_path_nested: expected nested dylib call to return 42" >&2
        echo "$dyld_executable_path_output" >&2
        exit 1
        ;;
esac
pass=$((pass + 1))
echo "PASS dylib_executable_path_nested"

# The entry is after its helper: decoding must cover code before LC_MAIN.
for mode in backward-thin backward-dyld; do
    "$TMPDIR/make_dyld_macho" "$mode" "$TMPDIR/$mode"
    backward_output="$("$BIN" launch "$TMPDIR/$mode")"
    case "$backward_output" in
        *"rax = 42"*) ;;
        *)
            echo "FAIL $mode: helper before entry did not return 42" >&2
            echo "$backward_output" >&2
            exit 1
            ;;
    esac
    pass=$((pass + 1))
    echo "PASS $mode"
done

cc -std=c17 -Wall -Wextra -Werror -O2 tests/wrap_fat_macho.c -o "$TMPDIR/wrap_fat_macho"
for endian in be le; do
    "$TMPDIR/wrap_fat_macho" "$endian" "$TMPDIR/backward-thin" "$TMPDIR/fat-$endian"
    fat_output="$("$BIN" launch "$TMPDIR/fat-$endian")"
    case "$fat_output" in
        *"rax = 42"*) ;;
        *)
            echo "FAIL fat-$endian: expected inspect and launch to handle FAT byte order" >&2
            echo "$fat_output" >&2
            exit 1
            ;;
    esac
    pass=$((pass + 1))
    echo "PASS fat-$endian"
done

cc -std=c17 -Wall -Wextra -Werror -O2 -Iinclude -Isrc \
    tests/test_jit.c src/a64_jit.c src/decode.c src/interp.c src/util.c src/native.c -lffi \
    -o "$TMPDIR/test_jit"
"$TMPDIR/test_jit"
pass=$((pass + 1))

cc -std=c17 -Wall -Wextra -Werror -O2 -Iinclude -Isrc \
    tests/test_setcc.c src/a64_jit.c src/decode.c src/interp.c src/util.c src/native.c -lffi \
    -o "$TMPDIR/test_setcc"
"$TMPDIR/test_setcc"
pass=$((pass + 1))

cc -std=c17 -Wall -Wextra -Werror -O2 -Iinclude -Isrc \
    tests/test_adc_sbb.c src/a64_jit.c src/decode.c src/interp.c src/util.c src/native.c -lffi \
    -o "$TMPDIR/test_adc_sbb"
"$TMPDIR/test_adc_sbb"
pass=$((pass + 1))

cc -std=c17 -Wall -Wextra -Werror -O2 -Iinclude -Isrc \
    tests/test_movext.c src/a64_jit.c src/decode.c src/interp.c src/util.c src/native.c -lffi \
    -o "$TMPDIR/test_movext"
"$TMPDIR/test_movext"
pass=$((pass + 1))

cc -std=c17 -Wall -Wextra -Werror -O2 -Iinclude -Isrc \
    tests/test_runtime.c src/a64_jit.c src/decode.c src/interp.c src/util.c src/native.c -lffi \
    -o "$TMPDIR/test_runtime"
"$TMPDIR/test_runtime"

cc -std=c17 -Wall -Wextra -Werror -O2 -Iinclude -Isrc \
    tests/test_atomic.c src/a64_jit.c src/decode.c src/interp.c src/util.c src/native.c -lffi \
    -o "$TMPDIR/test_atomic"
"$TMPDIR/test_atomic"

cc -std=c17 -Wall -Wextra -Werror -O2 -Iinclude -Isrc \
    tests/test_jvm_bridge.c src/decode.c src/interp.c src/util.c src/native.c -lffi \
    -o "$TMPDIR/test_jvm_bridge"
"$TMPDIR/test_jvm_bridge"

cc -std=c17 -Wall -Wextra -Werror -O2 -Iinclude -Isrc \
    tests/test_dynamic_code.c src/decode.c src/interp.c src/util.c src/native.c -lffi \
    -o "$TMPDIR/test_dynamic_code"
"$TMPDIR/test_dynamic_code"
pass=$((pass + 1))

cc -std=c17 -Wall -Wextra -Werror -O2 -Iinclude -Isrc \
    tests/test_x87.c src/decode.c src/interp.c src/util.c src/native.c -lffi \
    -o "$TMPDIR/test_x87"
"$TMPDIR/test_x87"
pass=$((pass + 1))

cc -std=c17 -Wall -Wextra -Werror -O2 -Iinclude -Isrc \
    tests/test_sse.c src/a64_jit.c src/decode.c src/interp.c src/util.c src/native.c -lffi \
    -o "$TMPDIR/test_sse"
"$TMPDIR/test_sse"
pass=$((pass + 1))

clang -target x86_64-apple-macos11 -O2 -fno-stack-protector -Wl,-no_fixup_chains \
    tests/jumps_guest.c -o "$TMPDIR/jumps_guest"
jumps_output="$("$BIN" launch --host-libs "$TMPDIR/jumps_guest")"
case "$jumps_output" in
    *"rax = 42"*) ;;
    *) echo "FAIL guest setjmp/longjmp: $jumps_output" >&2; exit 1 ;;
esac
pass=$((pass + 1))
echo "PASS guest setjmp/longjmp: nested frames, zero and negative return values"

cc -target x86_64-apple-macos11 -O1 -fno-stack-protector -dynamiclib \
    tests/constructors_library.c -Wl,-install_name,@rpath/libconstructors.dylib \
    -Wl,-no_fixup_chains -o "$TMPDIR/libconstructors.dylib"
cc -target x86_64-apple-macos11 -O1 -fno-stack-protector \
    tests/constructors_guest.c -L"$TMPDIR" -lconstructors -Wl,-rpath,@loader_path \
    -Wl,-no_fixup_chains -o "$TMPDIR/constructors_guest"
constructors_output="$("$BIN" launch --host-libs "$TMPDIR/constructors_guest" -- fixture)"
constructors_lines="$(printf '%s\n' "$constructors_output" | grep '^R2_INIT')"
expected_constructors='R2_INIT main: 123
R2_INIT main exit: 1234
R2_INIT C++ exit: 12345
R2_INIT library exit: 12345'
if [ "$constructors_lines" != "$expected_constructors" ]; then
    echo "FAIL guest constructors/destructors order: $constructors_output" >&2
    exit 1
fi
case "$constructors_output" in
    *"rax = 42"*) ;;
    *) echo "FAIL destructor overwrote main result" >&2; exit 1 ;;
esac
pass=$((pass + 1))
echo "PASS guest constructors, dependency order, argv and C++/C exit callbacks"

cc -target x86_64-apple-macos11 -O1 -fno-stack-protector \
    tests/constructors_exit.c -Wl,-no_fixup_chains -o "$TMPDIR/constructors_exit"
init_exit_output="$("$BIN" launch --host-libs "$TMPDIR/constructors_exit" -- normal)"
case "$init_exit_output" in
    *"R2_INIT_EXIT callback"*"rax = 23"*) ;;
    *) echo "FAIL exit from initializer: $init_exit_output" >&2; exit 1 ;;
esac
for exit_mode in immediate libc-immediate; do
init_exit_output="$("$BIN" launch --host-libs "$TMPDIR/constructors_exit" -- "$exit_mode")"
case "$init_exit_output" in
    *"R2_INIT_EXIT callback"*) echo "FAIL immediate exit ran callbacks" >&2; exit 1 ;;
    *"rax = 24"*) ;;
    *) echo "FAIL immediate exit from initializer: $init_exit_output" >&2; exit 1 ;;
esac
done
pass=$((pass + 1))
echo "PASS initializer exit stops constructors/main; immediate exit skips callbacks"

cc -target x86_64-apple-macos11 -std=c17 -O1 -fno-stack-protector \
    tests/tls_guest.c -Wl,-no_fixup_chains -o "$TMPDIR/tls_guest"
tls_output="$("$BIN" launch --host-libs "$TMPDIR/tls_guest")"
case "$tls_output" in
    *"R2_TLS data, zero-fill, rebased pointer, alignment and register preservation: OK"*"rax = 42"*) ;;
    *) echo "FAIL guest TLS: $tls_output" >&2; exit 1 ;;
esac
pass=$((pass + 1))
echo "PASS guest Mach-O TLS descriptors and templates"

mkdir -p "$TMPDIR/dlopen" "$TMPDIR/dlopen-broken" "$TMPDIR/dlopen-app"
cc -target x86_64-apple-macos11 -O1 -fno-stack-protector -dynamiclib \
    tests/dlopen_leaf.c -Wl,-install_name,@loader_path/libdlopenleaf.dylib \
    -Wl,-no_fixup_chains -o "$TMPDIR/dlopen/libdlopenleaf.dylib"
cc -target x86_64-apple-macos11 -O1 -fno-stack-protector -dynamiclib \
    tests/dlopen_library.c -L"$TMPDIR/dlopen" -ldlopenleaf \
    -Wl,-install_name,@loader_path/libdlopen.dylib -Wl,-no_fixup_chains \
    -o "$TMPDIR/dlopen/libdlopen.dylib"
cp "$TMPDIR/dlopen/libdlopen.dylib" "$TMPDIR/dlopen-broken/libbroken.dylib"
cc -target x86_64-apple-macos11 -O1 -fno-stack-protector -dynamiclib \
    tests/dlopen_extra.c -Wl,-no_fixup_chains -o "$TMPDIR/dlopen/libextra.dylib"
cc -target x86_64-apple-macos11 -O1 -fno-stack-protector \
    tests/dlopen_guest.c -Wl,-rpath,@executable_path/../dlopen -Wl,-no_fixup_chains -o "$TMPDIR/dlopen-app/main"
dlopen_output="$("$BIN" launch --host-libs "$TMPDIR/dlopen-app/main" -- \
    "$TMPDIR/dlopen/libdlopen.dylib" "$TMPDIR/dlopen/libextra.dylib" \
    "$TMPDIR/dlopen-broken/libbroken.dylib")"
dlopen_lines="$(printf '%s\n' "$dlopen_output" | grep '^R2_DLOPEN')"
expected_dlopen='R2_DLOPEN init
R2_DLOPEN guest code and scopes: OK
R2_DLOPEN exit'
if [ "$dlopen_lines" != "$expected_dlopen" ]; then
    echo "FAIL guest dlopen: $dlopen_output" >&2
    exit 1
fi
pass=$((pass + 1))
echo "PASS guest dlopen/dlsym/dladdr/dlclose, constructors, scopes and rollback"

python3 tests/test_weak_bind.py "$BIN"
pass=$((pass + 1))

clang -target x86_64-apple-macos11 -O2 -fno-stack-protector -Wl,-no_fixup_chains \
    tests/threads_guest.c -o "$TMPDIR/threads_guest"
python3 tests/test_threads.py "$BIN" "$TMPDIR/threads_guest"
pass=$((pass + 1))

cc -std=c17 -Wall -Wextra -Werror -O2 -Iinclude -Isrc \
    tests/test_cpu.c src/decode.c src/interp.c src/util.c src/native.c -lffi -o "$TMPDIR/test_cpu"
"$TMPDIR/test_cpu"
pass=$((pass + 1))
clang -target x86_64-apple-macos11 -O1 -fno-stack-protector -Wl,-no_fixup_chains \
    tests/zlib_guest.c -lz -o "$TMPDIR/zlib_guest"
python3 tests/test_zlib.py "$BIN" "$TMPDIR/zlib_guest"
pass=$((pass + 1))

clang -target x86_64-apple-macos11 -O1 -fno-stack-protector -Wl,-no_fixup_chains \
    tests/signals_guest.c -o "$TMPDIR/signals_guest"
python3 tests/test_signals.py "$BIN" "$TMPDIR/signals_guest"
pass=$((pass + 1))
python3 tests/test_thread_signals.py "$BIN"
pass=$((pass + 1))
clang -target x86_64-apple-macos11 -O1 -fno-stack-protector -Wl,-no_fixup_chains \
    tests/metrics_guest.c -o "$TMPDIR/metrics_guest"
metrics_output="$("$BIN" launch --host-libs "$TMPDIR/metrics_guest")"
case "$metrics_output" in
    *"PASS guest Mach task/host and process metrics ABI"*"rax = 0 "*) ;;
    *) echo "FAIL guest metrics: $metrics_output" >&2; exit 1 ;;
esac
pass=$((pass + 1))
echo "PASS guest Mach task/host and process metrics ABI"

clang -target x86_64-apple-macos11 -O1 -fno-stack-protector -Wl,-no_fixup_chains \
    tests/framework_guest.c -framework CoreFoundation -framework CoreServices -framework AudioToolbox -framework CoreAudio -o "$TMPDIR/framework_guest"
python3 tests/test_framework.py "$BIN" "$TMPDIR/framework_guest"
pass=$((pass + 1))

echo "$pass test(s) passed"
