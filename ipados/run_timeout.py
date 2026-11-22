#!/usr/bin/env python3
"""Run a command with a wall-clock timeout.

Usage: run_timeout.py <seconds> <command> [args...]

Exit status is the child's status, or 124 on timeout (the child is killed).
This exists because several `xcrun simctl` calls hang indefinitely on the
Intel-host iOS 26.3 simulator used for this port.
"""
import subprocess
import sys


def main() -> int:
    if len(sys.argv) < 3:
        print("usage: run_timeout.py <seconds> <command> [args...]", file=sys.stderr)
        return 2
    try:
        seconds = float(sys.argv[1])
    except ValueError:
        print("error: timeout must be a number", file=sys.stderr)
        return 2
    command = sys.argv[2:]
    proc = subprocess.Popen(command)
    try:
        return proc.wait(timeout=seconds)
    except subprocess.TimeoutExpired:
        proc.kill()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            pass
        print("TIMEOUT after %gs: %s" % (seconds, " ".join(command)), file=sys.stderr)
        return 124


if __name__ == "__main__":
    sys.exit(main())
