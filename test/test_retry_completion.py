#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Compile the actual completed() method, not a second copy of its logic.

Default: C++17 control-flow doubles, no Qt/Polkit required.
--qt: real QMessageBox (including Escape/close) and QPointer; needs Qt6Widgets.
Neither mode invokes PAM or tests the full agent/listener implementation.
"""
import argparse
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qt', action='store_true')
    parser.add_argument('--source', type=Path, help='Alternate source for regression checks')
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    source_path = args.source or here.parent / 'src' / 'policykitagent.cpp'
    source = source_path.read_text()
    signature = 'void PolicykitAgent::completed(bool gainedAuthorization)'
    start = source.index(signature)
    end = source.index('\nvoid PolicykitAgent::showError(', start)
    constant = re.search(r'constexpr int maximumAuthenticationAttempts\s*=\s*\d+;', source)
    if not constant:
        parser.error('Cannot find the production attempt limit')
    implementation = constant[0] + '\n' + source[start:end]
    harness = (here / 'test_retry_completion.cpp').read_text()
    assert harness.count('// PRODUCTION_IMPLEMENTATION') == 1
    harness = harness.replace('// PRODUCTION_IMPLEMENTATION', implementation)
    compiler = shlex.split(os.environ.get('CXX', 'c++'))
    flags = ['-std=c++17', '-Wall', '-Wextra', '-Werror', '-g']
    flags += shlex.split(os.environ.get('CXXFLAGS', ''))
    libraries = []
    env = dict(os.environ)
    with tempfile.TemporaryDirectory(prefix='lxqt-retry-test-') as tmp:
        cpp = Path(tmp) / 'test_retry_completion.cpp'
        cpp.write_text(harness)
        if args.qt:
            flags += ['-DUSE_QT', '-fPIC']
            flags += shlex.split(subprocess.check_output(
                ['pkg-config', '--cflags', 'Qt6Widgets'], text=True))
            libraries = shlex.split(subprocess.check_output(
                ['pkg-config', '--libs', 'Qt6Widgets'], text=True))
            moc = os.environ.get('MOC') or shutil.which('moc6')
            if not moc:
                for candidate in ('/usr/lib/qt6/libexec/moc', '/usr/lib64/qt6/libexec/moc'):
                    if Path(candidate).is_file():
                        moc = candidate
                        break
            if not moc:
                parser.error('Qt 6 moc not found; set MOC to its executable path')
            moc_flags = [flag for flag in flags if flag.startswith(('-I', '-D'))]
            subprocess.run([moc, *moc_flags, str(cpp), '-o',
                            str(Path(tmp) / 'test_retry_completion.moc')], check=True)
            env.setdefault('QT_QPA_PLATFORM', 'offscreen')
        binary = Path(tmp) / 'test_retry_completion'
        subprocess.run([*compiler, *flags, str(cpp), '-o', str(binary), *libraries], check=True)
        subprocess.run([str(binary)], check=True, timeout=30, env=env)


if __name__ == '__main__':
    main()
