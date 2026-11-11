import subprocess
import sys

binary, guest = sys.argv[1:]
for arguments in ([], ["unhandled"], ["bad-context"]):
    result = subprocess.run(
        [binary, "launch", "--host-libs", guest, "--", *arguments],
        capture_output=True,
        text=True,
        timeout=20,
    )
    if not arguments:
        if (
            result.returncode != 0
            or "PASS guest Darwin x86 signals" not in result.stdout
            or "rax = 0 " not in result.stdout
        ):
            raise SystemExit(result.stdout + result.stderr)
    else:
        expected = (
            "not readable"
            if arguments == ["unhandled"]
            else "unsupported guest mcontext size"
        )
        if result.returncode == 0 or expected not in result.stderr:
            raise SystemExit(
                "Unexpected guest signal failure: " + result.stdout + result.stderr
            )
print(
    "PASS guest x86 fault recovery, red zone, context changes, async signals and invalid contexts"
)
