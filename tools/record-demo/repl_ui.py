#!/usr/bin/env python3
# lambdock -- hyper-hackable desktop dock for Wayland
#
# SPDX-License-Identifier: MIT
# Copyright © Josep Bigorra <jjbigorra@gmail.com>

"""A tiny terminal front-end for the lambdock Guile socket REPL.

The demo controller writes expressions to a FIFO.  This program types them out
character by character, sends them to the real REPL socket, prints the answer,
and records the command index in a state file so the controller can wait for
each expression to finish before moving on.

    repl_ui.py --socket PATH --fifo PATH --done PATH --ready PATH
"""

from __future__ import annotations

import argparse
import os
import select
import signal
import socket
import sys
import time

RESET = "\x1b[0m"
DIM = "\x1b[2m"
BOLD = "\x1b[1m"
GREEN = "\x1b[1;32m"
CYAN = "\x1b[1;36m"
WHITE = "\x1b[0;37m"
RED = "\x1b[1;31m"
PROMPT = "scheme@(guile-user)> "
BANNER_END = PROMPT.encode()


def write_atomic(path: str, value: str) -> None:
    tmp = f"{path}.tmp"
    with open(tmp, "w", encoding="utf-8") as handle:
        handle.write(value)
    os.replace(tmp, path)


def connect_with_retry(path: str, deadline: float = 15.0) -> socket.socket:
    start = time.monotonic()
    last_error: Exception | None = None
    while time.monotonic() - start < deadline:
        try:
            sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            sock.connect(path)
            return sock
        except OSError as error:
            last_error = error
            time.sleep(0.1)
    raise RuntimeError(f"could not connect to REPL at {path}: {last_error}")


def read_until_prompt(sock: socket.socket, timeout: float = 5.0) -> bytes:
    data = bytearray()
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        ready, _, _ = select.select([sock], [], [], 0.05)
        if not ready:
            continue
        chunk = sock.recv(65536)
        if not chunk:
            break
        data.extend(chunk)
        if data.endswith(BANNER_END):
            break
    return bytes(data)


def strip_prompt(data: bytes) -> str:
    text = data.decode("utf-8", "replace")
    marker = PROMPT
    while marker in text:
        text = text.replace(marker, "")
    return text.strip()


def type_out(text: str, delay_ms: int) -> None:
    for character in text:
        sys.stdout.write(CYAN + character + RESET)
        sys.stdout.flush()
        if delay_ms > 0:
            time.sleep(delay_ms / 1000.0)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--socket", required=True)
    parser.add_argument("--fifo", required=True)
    parser.add_argument("--done", required=True)
    parser.add_argument("--ready", required=True)
    args = parser.parse_args()

    def on_signal(signum, frame):  # noqa: ANN001
        raise SystemExit(0)

    signal.signal(signal.SIGTERM, on_signal)
    signal.signal(signal.SIGINT, on_signal)

    sock = connect_with_retry(args.socket)
    sock.setblocking(False)
    read_until_prompt(sock)  # discard Guile's copyright banner

    sys.stdout.write(
        f"{DIM};; lambdock live REPL -- Guile inside the running dock{RESET}\n"
        f"{DIM};; eval Scheme; the dock changes where it stands{RESET}\n\n"
    )
    sys.stdout.flush()
    write_atomic(args.ready, "1")

    index = 0
    write_atomic(args.done, "0")
    with open(args.fifo, "r", encoding="utf-8", buffering=1) as fifo:
        for line in fifo:
            line = line.rstrip("\n")
            if not line:
                continue
            try:
                speed_text, expression = line.split("\t", 1)
                speed = int(speed_text)
            except ValueError:
                speed, expression = 20, line

            sys.stdout.write(GREEN + PROMPT + RESET)
            sys.stdout.flush()
            type_out(expression, speed)
            sys.stdout.write("\n")
            sys.stdout.flush()

            sock.sendall(expression.encode() + b"\n")
            response = strip_prompt(read_until_prompt(sock))
            if "Entering a new prompt" in response or "scheme@(guile-user) [" in response:
                # Leave the debugger so the next expression still works.
                sock.sendall(b",q\n")
                response += "\n" + strip_prompt(read_until_prompt(sock))
            if response:
                color = RED if "error" in response.lower() else WHITE
                for response_line in response.splitlines():
                    sys.stdout.write(color + response_line + RESET + "\n")
            sys.stdout.flush()

            index += 1
            write_atomic(args.done, str(index))

    sys.stdout.write(DIM + ";; EOF" + RESET + "\n")
    sys.stdout.flush()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
