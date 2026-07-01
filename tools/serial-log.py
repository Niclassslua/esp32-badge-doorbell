#!/usr/bin/env python3
"""
Small macOS-friendly serial monitor for badge firmware logs.

Default connection matches:

    screen /dev/cu.usbserial-110 115200

Shortcuts while connected:

    Ctrl-Y  copy all received serial output to the macOS clipboard
    Ctrl-]  exit
"""

from __future__ import annotations

import argparse
import errno
import os
import select
import shutil
import signal
import subprocess
import sys
import termios
import time
import tty


DEFAULT_PORT = "/dev/cu.usbserial-110"
DEFAULT_BAUD = 115200
COPY_KEY = b"\x19"  # Ctrl-Y
EXIT_KEY = b"\x1d"  # Ctrl-]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Serial monitor with terminal scrollback and Ctrl-Y clipboard capture."
    )
    parser.add_argument(
        "port",
        nargs="?",
        default=DEFAULT_PORT,
        help=f"serial device path (default: {DEFAULT_PORT})",
    )
    parser.add_argument(
        "-b",
        "--baud",
        type=int,
        default=DEFAULT_BAUD,
        help=f"baud rate (default: {DEFAULT_BAUD})",
    )
    parser.add_argument(
        "--no-wait",
        action="store_true",
        help="fail immediately if the serial device is missing",
    )
    return parser.parse_args()


def baud_constant(baud: int) -> int:
    name = f"B{baud}"
    if not hasattr(termios, name):
        raise SystemExit(f"Unsupported baud rate for this platform: {baud}")
    return getattr(termios, name)


def wait_for_port(path: str, no_wait: bool) -> None:
    if os.path.exists(path):
        return
    if no_wait:
        raise SystemExit(f"Serial device not found: {path}")

    print(f"Waiting for serial device {path} ...", file=sys.stderr)
    while not os.path.exists(path):
        time.sleep(0.5)


def open_serial(path: str, baud: int) -> int:
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    try:
        attrs = termios.tcgetattr(fd)
        speed = baud_constant(baud)

        attrs[0] = 0
        attrs[1] = 0
        attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
        attrs[3] = 0
        attrs[4] = speed
        attrs[5] = speed
        attrs[6][termios.VMIN] = 0
        attrs[6][termios.VTIME] = 0

        if hasattr(termios, "CRTSCTS"):
            attrs[2] &= ~termios.CRTSCTS

        termios.tcsetattr(fd, termios.TCSANOW, attrs)
        return fd
    except Exception:
        os.close(fd)
        raise


def connect_serial(path: str, baud: int, no_wait: bool) -> int:
    announced = False
    retry_errors = {errno.ENOENT, errno.ENXIO, errno.EBUSY, errno.EIO}

    while True:
        wait_for_port(path, no_wait)
        try:
            return open_serial(path, baud)
        except OSError as exc:
            if no_wait or exc.errno not in retry_errors:
                raise
            if not announced:
                print(
                    f"Waiting for serial device {path} to become available ...",
                    file=sys.stderr,
                )
                announced = True
            time.sleep(0.5)


def status(message: str) -> None:
    sys.stderr.write(f"\r\n[{message}]\r\n")
    sys.stderr.flush()


def copy_to_clipboard(captured: bytearray) -> None:
    if not shutil.which("pbcopy"):
        status("pbcopy is not available; clipboard copy failed")
        return

    text = bytes(captured).decode("utf-8", errors="replace")
    subprocess.run(["pbcopy"], input=text.encode("utf-8"), check=False)
    status(f"copied {len(captured)} bytes of serial output to clipboard")


def monitor(port: str, baud: int, no_wait: bool) -> int:
    serial_fd = connect_serial(port, baud, no_wait)
    stdin_fd = sys.stdin.fileno()
    stdout_fd = sys.stdout.fileno()
    old_stdin_attrs = termios.tcgetattr(stdin_fd)
    captured = bytearray()

    print(
        f"Connected to {port} at {baud}. Ctrl-Y copies captured output; Ctrl-] exits.",
        file=sys.stderr,
    )

    try:
        tty.setraw(stdin_fd)
        while True:
            readable, _, _ = select.select([serial_fd, stdin_fd], [], [])

            if serial_fd in readable:
                try:
                    data = os.read(serial_fd, 4096)
                except OSError as exc:
                    status(f"serial read failed: {exc}")
                    return 1

                if not data:
                    status("serial device closed")
                    return 0

                captured.extend(data)
                os.write(stdout_fd, data)

            if stdin_fd in readable:
                key = os.read(stdin_fd, 1)
                if key == COPY_KEY:
                    copy_to_clipboard(captured)
                elif key == EXIT_KEY:
                    status("disconnecting")
                    return 0
                elif key:
                    os.write(serial_fd, key)
    finally:
        termios.tcsetattr(stdin_fd, termios.TCSADRAIN, old_stdin_attrs)
        os.close(serial_fd)


def main() -> int:
    signal.signal(signal.SIGPIPE, signal.SIG_DFL)
    args = parse_args()
    return monitor(args.port, args.baud, args.no_wait)


if __name__ == "__main__":
    raise SystemExit(main())
