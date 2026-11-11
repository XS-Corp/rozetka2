import os
import subprocess
import sys

binary, guest = sys.argv[1:]
for arguments in ([], ["unsupported"], ["callbacks"], ["audio-callbacks"]):
    result = subprocess.run(
        [binary, "launch", "--host-libs", guest, "--", *arguments],
        env=dict(os.environ, R2_JVM_PROBE="1"),
        capture_output=True,
        text=True,
        timeout=20,
    )
    if not arguments:
        if (
            result.returncode != 0
            or "PASS guest framework function stubs" not in result.stdout
            or "rax = 0 " not in result.stdout
        ):
            raise SystemExit(result.stdout + result.stderr)
    else:
        expected = (
            "AudioUnit is not owned by guest runtime"
            if arguments == ["audio-callbacks"]
            else (
                "guest CFArray callbacks are unsupported"
                if arguments == ["callbacks"]
                else "no ABI declaration for _CFBundleGetAllBundles"
            )
        )
        if result.returncode == 0 or expected not in result.stderr:
            raise SystemExit(
                "Unsafe framework procedure/callback was accepted: "
                + result.stdout
                + result.stderr
            )
print(
    "PASS framework procedure stubs, guest image enumeration and foreign va_list conversion"
)
