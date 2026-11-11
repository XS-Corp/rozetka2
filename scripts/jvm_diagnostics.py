#!/usr/bin/env python3
"""Send a diagnostic command to an already listening local guest JVM.

Uses HotSpot's architecture-independent Attach v1 protocol, as implemented by
OpenJDK's HotSpotVirtualMachine.writeCommand. It never launches a host JVM or
sends signals to start a listener.
"""

import argparse
import os
from pathlib import Path
import socket
import stat

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("pid", type=int)
parser.add_argument("command", nargs="?", default="Thread.print -l")
parser.add_argument("--output", type=Path)
options = parser.parse_args()
if options.pid <= 0 or "\0" in options.command:
    parser.error("Invalid PID or command")
paths = [
    Path(os.environ.get("TMPDIR", "/tmp")) / f".java_pid{options.pid}",
    Path("/tmp") / f".java_pid{options.pid}",
]
endpoint = next((path for path in paths if path.exists()), None)
if endpoint is None:
    raise SystemExit("The target JVM has no listening Attach socket")
metadata = endpoint.stat()
if not stat.S_ISSOCK(metadata.st_mode) or metadata.st_uid != os.geteuid():
    raise SystemExit("Attach socket is not owned by the current user")
request = (
    b"\0".join(value.encode() for value in ["1", "jcmd", options.command, "", ""])
    + b"\0"
)
with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
    connection.settimeout(30)
    connection.connect(str(endpoint))
    connection.sendall(request)
    output = bytearray()
    while True:
        chunk = connection.recv(65536)
        if not chunk:
            break
        output.extend(chunk)
        if len(output) > 16 * 1024 * 1024:
            raise SystemExit("Diagnostic output exceeded 16 MiB")
status, separator, body = output.partition(b"\n")
if not separator or status != b"0":
    raise SystemExit(output.decode(errors="replace"))
if options.output:
    options.output.parent.mkdir(parents=True, exist_ok=True)
    options.output.write_bytes(body)
    print(f"Saved {options.command}: {options.output}")
else:
    print(body.decode(errors="replace"), end="")
