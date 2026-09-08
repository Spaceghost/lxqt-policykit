#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Build the full agent lifecycle tests with real Qt and PolkitQt.

Registration/native helper transport are controlled test doubles, not PAM.
--qt is retained for compatibility; all runs now use actual Qt widgets.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qt', action='store_true', help=argparse.SUPPRESS)
    parser.add_argument('--source-tree', type=Path, help='Alternate complete agent checkout')
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--case', help='CTest regular expression selecting scenarios')
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    source = (args.source_tree or here.parent).resolve()
    with tempfile.TemporaryDirectory(prefix='lxqt-lifecycle-') as tmp:
        subprocess.run(['cmake', '-S', str(here / 'lifecycle'), '-B', tmp,
                        '-DCMAKE_BUILD_TYPE=Debug', f'-DAGENT_SOURCE_DIR={source}',
                        f'-DENABLE_SANITIZERS={"ON" if args.sanitize else "OFF"}'],
                       check=True, timeout=120)
        subprocess.run(['cmake', '--build', tmp, '--parallel', '2'], check=True, timeout=180)
        command = ['ctest', '--test-dir', tmp, '--output-on-failure', '--no-tests=error']
        if args.case:
            command += ['-R', args.case]
        env = dict(os.environ, QT_QPA_PLATFORM='offscreen')
        subprocess.run(command, check=True, timeout=360, env=env)


if __name__ == '__main__':
    main()
