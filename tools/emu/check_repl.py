#!/usr/bin/env python3
"""Fail unless every required marker appears in an esp-emu UART log.

Used by tools/emu/run_emu_ci.sh: the markers prove the loader verified the
signed bundle, MicroPython booted, and the injected REPL commands executed.

Usage: check_repl.py <log> <marker> [marker ...]
"""
import sys


def main(argv):
    if len(argv) < 2:
        print("usage: check_repl.py <log> <marker> [marker ...]", file=sys.stderr)
        return 2
    path, *markers = argv
    try:
        with open(path, "r", errors="replace") as f:
            text = f.read()
    except OSError as exc:
        print(f"check_repl: cannot read {path}: {exc}", file=sys.stderr)
        return 2

    missing = [m for m in markers if m not in text]
    if missing:
        print(f"check_repl: FAIL — {len(missing)} of {len(markers)} marker(s) "
              f"missing from {path}:", file=sys.stderr)
        for m in missing:
            print(f"  - {m!r}", file=sys.stderr)
        print("--- last 20 log lines ---", file=sys.stderr)
        print("\n".join(text.splitlines()[-20:]), file=sys.stderr)
        return 1

    print(f"check_repl: PASS — all {len(markers)} marker(s) present in {path}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
