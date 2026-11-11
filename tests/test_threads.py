import subprocess
import sys

binary, guest = sys.argv[1:]
for mode in ([], ["fail"], ["once-fail"], ["resolver-fail"], ["socketpair-fail"]):
    result = subprocess.run(
        [binary, "launch", "--host-libs", guest, "--", *mode],
        capture_output=True,
        text=True,
        timeout=20,
    )
    if not mode:
        if result.returncode != 0 or "PASS native guest pthreads" not in result.stdout:
            raise SystemExit(result.stdout + result.stderr)
    elif (
        result.returncode == 0
        or (
            "not writable"
            if mode == ["socketpair-fail"]
            else (
                "unowned resolver result"
                if mode == ["resolver-fail"]
                else "unsupported 0f opcode 0x0b"
            )
        )
        not in result.stderr
    ):
        raise SystemExit(
            "thread failure was not propagated: " + result.stdout + result.stderr
        )
print("PASS native pthreads and coordinated shutdown after guest thread failure")
