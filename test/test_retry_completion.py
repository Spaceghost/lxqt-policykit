#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Build full-agent lifecycle tests against an explicit production checkout.

The native transport and registration are simulated; Qt and PolkitQt are real.
"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile


def execute(command, log, *, timeout, env=None):
    """Preserve complete output and propagate all failures, including timeouts."""
    try:
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, timeout=timeout, env=env)
    except subprocess.TimeoutExpired as error:
        output = error.stdout or ""
        if isinstance(output, bytes):
            output = output.decode("utf-8", errors="replace")
        if log:
            log.write_text(output, encoding="utf-8")
        print(output, end="", flush=True)
        raise
    if log:
        log.write_text(result.stdout, encoding="utf-8")
    print(result.stdout, end="", flush=True)
    result.check_returncode()
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qt", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--source-tree", type=Path, help="Complete production checkout")
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--build-type", choices=("Debug", "Release", "RelWithDebInfo"), default="Debug")
    parser.add_argument("--results-dir", type=Path, help="Retain logs, test inventory and metadata")
    parser.add_argument("--case", help="CTest regular expression selecting scenarios")
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    source = (args.source_tree or here.parent).resolve(strict=True)
    if not (source / "src/policykitagent.cpp").is_file():
        parser.error("source tree does not contain src/policykitagent.cpp")
    results = args.results_dir.resolve() if args.results_dir else None
    if results:
        results.mkdir(parents=True, exist_ok=True)

    def log(name):
        return results / name if results else None

    revision = subprocess.run(
        ["git", "-c", f"safe.directory={source}", "-C", str(source), "rev-parse", "HEAD"],
        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, timeout=15,
    )
    metadata = {
        "source": str(source), "revision": revision.stdout.strip() if revision.returncode == 0 else None,
        "compiler": os.environ.get("CXX", "default"), "build_type": args.build_type,
        "sanitizers": args.sanitize, "case_selection": args.case,
        "backend": "deterministic native transport; real Qt/PolkitQt",
        "status": "started",
    }
    with tempfile.TemporaryDirectory(prefix="lxqt-lifecycle-") as tmp:
        try:
            execute(["cmake", "-S", str(here / "lifecycle"), "-B", tmp,
                     f"-DCMAKE_BUILD_TYPE={args.build_type}", f"-DAGENT_SOURCE_DIR={source}",
                     f'-DENABLE_SANITIZERS={"ON" if args.sanitize else "OFF"}'],
                    log("configure.log"), timeout=120)
            execute(["cmake", "--build", tmp, "--parallel", "2"], log("build.log"), timeout=180)
            execute(["ctest", "--test-dir", tmp, "--show-only=json-v1"],
                    log("test-inventory.json"), timeout=30)
            command = ["ctest", "--test-dir", tmp, "--output-on-failure", "--no-tests=error"]
            if args.case:
                command += ["-R", args.case]
            if results:
                version = subprocess.check_output(["ctest", "--version"], text=True, timeout=15)
                match = re.search(r"(\d+)\.(\d+)\.", version)
                if match and tuple(map(int, match.groups())) >= (3, 21):
                    command += ["--output-junit", str(results / "results.xml")]
            execute(command, log("tests.log"), timeout=600,
                    env=dict(os.environ, QT_QPA_PLATFORM="offscreen"))
            metadata["status"] = "passed"
        except Exception as error:
            metadata["status"] = "failed"
            metadata["failure"] = str(error)
            raise
        finally:
            if results:
                (results / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
                last_log = Path(tmp) / "Testing/Temporary/LastTest.log"
                if last_log.is_file():
                    (results / "LastTest.log").write_bytes(last_log.read_bytes())


if __name__ == "__main__":
    main()
