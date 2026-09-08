#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Compile every negative control successfully, then require runtime failures."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
BASELINE = 'aab7234ca7d0092cdf6e9afde6baab70519cbafc'


def run_case(source, name, case):
    build = source.parent / (name + '-build')
    subprocess.run(['cmake', '-S', str(ROOT / 'test/lifecycle'), '-B', str(build),
                    '-DCMAKE_BUILD_TYPE=Debug', f'-DAGENT_SOURCE_DIR={source}'],
                   check=True, timeout=120)
    subprocess.run(['cmake', '--build', str(build), '--parallel', '2'], check=True, timeout=180)
    outcome = subprocess.run(['ctest', '--test-dir', str(build), '--output-on-failure',
                              '--no-tests=error', '-R', '^' + case + '$'],
                             env=dict(os.environ, QT_QPA_PLATFORM='offscreen'), timeout=30)
    if outcome.returncode == 0:
        raise RuntimeError(f'Negative control survived: {name}')
    if outcome.returncode != 8:
        raise RuntimeError(f'CTest infrastructure failure, not an assertion: {name}: {outcome.returncode}')
    print(f'REJECTED at runtime: {name}', flush=True)


def main():
    with tempfile.TemporaryDirectory(prefix='lxqt-negative-') as tmp:
        root = Path(tmp)
        baseline = root / 'baseline'
        baseline.mkdir()
        # Only known source paths are materialized; do not extract untrusted archives.
        (baseline / 'src').mkdir()
        for path in ('policykitagent.cpp', 'policykitagent.h', 'policykitagentgui.cpp',
                     'policykitagentgui.h', 'policykitagentgui.ui'):
            content = subprocess.check_output(['git', '-C', str(ROOT), 'show',
                                               f'{BASELINE}:src/{path}'], timeout=30)
            (baseline / 'src' / path).write_bytes(content)
        run_case(baseline, 'previous-cancel-does-not-complete', 'external_cancel_password')
        run_case(baseline, 'previous-native-callback-blocked', 'native_callback_boundary')
        original = (ROOT / 'src/policykitagent.cpp').read_text()
        mutations = [
            ('ignore-cancel', 'choice == QMessageBox::Ok && !m_userCancelled',
             'choice != QMessageBox::NoButton && !m_userCancelled', 'cancel_retry'),
            ('drop-cancel-completion',
             'm_userCancelled = true;\n    finishAuthentication(m_result);',
             'm_userCancelled = true;\n    // Deliberately omit completion.', 'external_cancel_password'),
            ('allow-stale-answer', 'if (m_requestId != requestId || m_result != result)\n                return;',
             'if (!m_result)\n                return;', 'stale_dialog'),
            ('allow-finished-callback-to-clear-request',
             'bool PolicykitAgent::initiateAuthenticationFinish()\n{',
             'bool PolicykitAgent::initiateAuthenticationFinish()\n{\n    m_inProgress = false;',
             'finish_callback'),
        ]
        for name, before, after, case in mutations:
            if original.count(before) != 1:
                raise RuntimeError(f'Mutation anchor changed: {name}')
            source = root / name
            shutil.copytree(ROOT / 'src', source / 'src')
            (source / 'src/policykitagent.cpp').write_text(original.replace(before, after, 1))
            run_case(source, name, case)


if __name__ == '__main__':
    main()
