#!/usr/bin/env python3
"""Run genuine x86_64 GNU nano via rozetka2, edit/save/reopen in a PTY."""

import argparse
import errno
import fcntl
import os
from pathlib import Path
import pty
import select
import signal
import struct
import termios
import tempfile
import time

parser = argparse.ArgumentParser()
parser.add_argument("translator", type=Path)
parser.add_argument("nano", type=Path)
parser.add_argument("--unicode", action="store_true")
args = parser.parse_args()
translator, nano = args.translator.resolve(), args.nano.resolve()
transcripts = []


class Editor:
    def __init__(self, path):
        ready, release = os.pipe()
        pid, master = pty.fork()
        if pid == 0:
            os.close(release)
            os.read(ready, 1)
            os.close(ready)
            os.environ.update(
                TERM="xterm-256color", LC_ALL="en_US.UTF-8" if args.unicode else "C"
            )
            os.environ.pop("R2_TRACE_IMPORTS", None)
            os.execv(
                str(translator),
                [str(translator), "launch", "--host-libs", str(nano), "--", str(path)],
            )
        self.pid, self.master, self.data, self.status = pid, master, bytearray(), None
        os.close(ready)
        fcntl.ioctl(master, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))
        self.before = termios.tcgetattr(master)
        os.write(release, b"1")
        os.close(release)

    def drain(self, timeout=0.1):
        ready, _, _ = select.select([self.master], [], [], timeout)
        if ready:
            try:
                chunk = os.read(self.master, 65536)
                if chunk:
                    self.data.extend(chunk)
            except OSError as error:
                if error.errno != errno.EIO:
                    raise
        if self.status is None:
            finished, status = os.waitpid(self.pid, os.WNOHANG)
            if finished:
                self.status = status

    def wait(self, text, timeout=20):
        deadline = time.monotonic() + timeout
        while text not in self.data:
            self.drain()
            if self.status is not None:
                raise AssertionError(
                    f"nano exited before {text!r}:\n{self.data.decode(errors='replace')[-4000:]}"
                )
            if time.monotonic() > deadline:
                raise AssertionError(
                    f"timeout waiting for {text!r}:\n{self.data.decode(errors='replace')[-4000:]}"
                )

    def send(self, data):
        os.write(self.master, data)

    def finish(self):
        deadline = time.monotonic() + 20
        while self.status is None and time.monotonic() < deadline:
            self.drain()
        assert self.status is not None, "nano did not exit"
        for _ in range(3):
            self.drain(0)
        assert os.waitstatus_to_exitcode(self.status) == 0, self.data.decode(
            errors="replace"
        )[-4000:]
        assert b"rax = 0" in self.data, "guest nano returned an error"
        after = termios.tcgetattr(self.master)
        assert (after[3] & (termios.ICANON | termios.ECHO)) == (
            self.before[3] & (termios.ICANON | termios.ECHO)
        ), "terminal mode not restored"
        transcripts.append(bytes(self.data))
        os.close(self.master)

    def cleanup(self):
        if self.status is None:
            os.kill(self.pid, signal.SIGKILL)
            os.waitpid(self.pid, 0)
        try:
            os.close(self.master)
        except OSError:
            pass


with tempfile.TemporaryDirectory(prefix="r2nano-", dir="/tmp") as directory:
    path = Path(directory) / "edited.txt"
    text = "nano runs through rozetka2"
    if args.unicode:
        text += " — Привет, мир!"
    editor = Editor(path)
    try:
        editor.wait(b"GNU nano 9.3")
        editor.wait(b"New File")
        editor.send(text.encode() + b"\rsecond line")
        editor.send(b"\x0f")
        editor.wait(b"Write to File:")
        editor.send(b"\r")
        editor.wait(b"Wrote 2 lines")
        assert (
            path.read_bytes() == (text + "\nsecond line\n").encode()
        ), "saved bytes differ from input"
        assert (
            path.stat().st_mode & 0o600 == 0o600
        ), "open variadic mode was not preserved"
        editor.send(b"\x18")
        editor.finish()
    finally:
        editor.cleanup()
    editor = Editor(path)
    try:
        editor.wait(b"GNU nano 9.3")
        editor.wait(b"second line")
        # Editing an existing file exercises libc stat/FILE layouts and readback.
        editor.send(b"\x01")  # Home / beginning of first line
        editor.send(b"verified: ")
        editor.send(b"\x0f")
        editor.wait(b"Write to File:")
        editor.send(b"\r")
        editor.wait(b"Wrote 2 lines")
        assert path.read_bytes() == ("verified: " + text + "\nsecond line\n").encode()
        editor.send(b"\x18")
        editor.finish()
    finally:
        editor.cleanup()

log = translator.parent / "nano-pty-transcript.log"
log.write_bytes(b"\n--- reopen ---\n".join(transcripts))
print(
    f"PASS nano PTY: open, type, save, permissions, exit, reopen, edit, save; terminal restored ({'UTF-8' if args.unicode else 'ASCII'})"
)
print(f"Transcript: {log}")
