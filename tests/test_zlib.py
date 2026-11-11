import subprocess
import sys

binary, guest = sys.argv[1:]
for arguments in ([], ["callback"]):
    result = subprocess.run(
        [binary, "launch", "--host-libs", guest, "--", *arguments],
        capture_output=True,
        text=True,
        timeout=20,
    )
    if not arguments:
        if result.returncode != 0 or "PASS guest zlib roundtrip" not in result.stdout:
            raise SystemExit(result.stdout + result.stderr)
    elif (
        result.returncode == 0
        or "guest zlib allocation callbacks are unsupported" not in result.stderr
    ):
        raise SystemExit(
            "unsafe zlib callback was accepted: " + result.stdout + result.stderr
        )
print(
    "PASS guest zlib stream lifecycle, roundtrip and rejection of untranslated callbacks"
)
