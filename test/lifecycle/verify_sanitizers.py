#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Require the expected sanitizer diagnostic and failure, never a silent pass."""
import os
from pathlib import Path
import subprocess
import sys


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: verify_sanitizers.py PROBE_EXECUTABLE")
    executable = Path(sys.argv[1]).resolve(strict=True)
    env = dict(os.environ)
    # UBSan runtime recovery must not override the shared compile-time flags.
    env.update(UBSAN_OPTIONS="halt_on_error=0", ASAN_OPTIONS="detect_leaks=0:halt_on_error=1")
    for scenario, diagnostic in (
        ("undefined", "runtime error: signed integer overflow"),
        ("address", "AddressSanitizer: heap-use-after-free"),
    ):
        result = subprocess.run(
            [str(executable), scenario], env=env, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, timeout=15,
        )
        print(result.stdout, end="", flush=True)
        if (result.returncode != 1 or diagnostic not in result.stdout
                or "SANITIZER_DID_NOT_STOP" in result.stdout):
            raise RuntimeError(f"Sanitizer enforcement failed: {scenario}, exit {result.returncode}")
        print(f"VERIFIED fatal sanitizer: {scenario}", flush=True)


if __name__ == "__main__":
    main()
