"""Compact live progress while subprocess output stays in the full test log."""

from __future__ import annotations

import os
import re
import signal
import subprocess
import sys
import time


_COMPLETED = re.compile(r"^\s*(\d+)\s*/\s*(\d+)\s+Test\s+#")


def run_logged(command, *, log, label, total=None, quiet=False) -> int:
    """Run a command and report elapsed time and observed CTest completions.

    A separate reader tails only this command's output. CTest output remains in
    the log, including failures. The terminal never implies a percentage for
    configure/build commands where no trustworthy total is available.
    """
    log.flush()
    start_offset = log.tell()
    started = time.monotonic()
    last_render = started
    completed = 0
    pending_line = ""
    frame = 0
    terminal = sys.stderr.isatty()
    previous_width = 0

    def render(*, final=False, returncode=None):
        nonlocal frame, previous_width
        if quiet:
            return
        elapsed = time.monotonic() - started
        dots = "." * (frame % 3 + 1)
        frame += 1
        progress = ""
        if total:
            progress = f" {completed}/{total}"
            if terminal:
                filled = min(20, completed * 20 // total)
                progress = " [" + "=" * filled + " " * (20 - filled) + "]" + progress
        state = f"done (exit {returncode})" if final else dots
        message = f"{label}{progress} {state} {elapsed:.0f}s"
        if terminal:
            padding = " " * max(0, previous_width - len(message))
            print("\r" + message + padding, end="\n" if final else "", file=sys.stderr, flush=True)
            previous_width = len(message)
        else:
            print(message, file=sys.stderr, flush=True)

    def observe(reader, *, final=False):
        nonlocal pending_line, completed
        pending_line += reader.read()
        lines = pending_line.split("\n")
        pending_line = lines.pop()
        if final:
            lines.append(pending_line)
            pending_line = ""
        for line in lines:
            match = _COMPLETED.match(line)
            if match:
                completed = max(completed, int(match.group(1)))
        # An arbitrary subprocess can emit a huge line; retain only a bounded
        # tail. CTest's completion lines are short and end in a newline.
        pending_line = pending_line[-8192:]

    with open(log.name, encoding="utf-8", errors="replace") as reader:
        reader.seek(start_offset)
        process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT,
                                   start_new_session=os.name != "nt")
        try:
            render()
            while True:
                try:
                    result = process.wait(timeout=1)
                except subprocess.TimeoutExpired:
                    observe(reader)
                    now = time.monotonic()
                    if now - last_render >= (1 if terminal else 5):
                        render()
                        last_render = now
                else:
                    observe(reader, final=True)
                    render(final=True, returncode=result)
                    return result
        except BaseException:
            # Do not release the caller's build lock while its child keeps
            # writing to shared CTest outputs after cancellation or an error.
            def stop(force=False):
                if os.name == "nt":
                    process.kill() if force else process.terminate()
                else:
                    try:
                        os.killpg(process.pid, signal.SIGKILL if force else signal.SIGTERM)
                    except ProcessLookupError:
                        pass

            stop()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                stop(force=True)
                process.wait()
            if terminal and not quiet:
                print(file=sys.stderr, flush=True)
            raise
