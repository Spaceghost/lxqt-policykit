#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Compile every negative control, then require its expected runtime assertion."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
BASELINE = 'aab7234ca7d0092cdf6e9afde6baab70519cbafc'


def run_case(source, name, case, expected_assertion):
    build = source.parent / (name + '-build')
    subprocess.run(['cmake', '-S', str(ROOT / 'test/lifecycle'), '-B', str(build),
                    '-DCMAKE_BUILD_TYPE=Debug', f'-DAGENT_SOURCE_DIR={source}'],
                   check=True, timeout=120)
    subprocess.run(['cmake', '--build', str(build), '--parallel', '2'],
                   check=True, timeout=180)
    outcome = subprocess.run([str(build / 'test_agent_lifecycle'), case],
                             env=dict(os.environ, QT_QPA_PLATFORM='offscreen',
                                      G_DEBUG='fatal-criticals'),
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             text=True, timeout=15)
    print(outcome.stdout, end='', flush=True)
    # CHECK prints its source line and stringified expression. Require the
    # intended assertion, not a compiler failure, crash, timeout, or other check.
    expected = r'(?m)^\d+: ' + re.escape(expected_assertion) + r'$'
    if outcome.returncode != 1 or not re.search(expected, outcome.stdout):
        raise RuntimeError(f'Negative control did not reach its expected assertion: '
                           f'{name}: exit {outcome.returncode}')
    print(f'REJECTED at runtime: {name}', flush=True)


def main():
    with tempfile.TemporaryDirectory(prefix='lxqt-negative-') as tmp:
        root = Path(tmp)
        baseline = root / 'baseline'
        baseline.mkdir()
        # Only known source paths are materialized; do not extract archives.
        (baseline / 'src').mkdir()
        for path in ('policykitagent.cpp', 'policykitagent.h', 'policykitagentgui.cpp',
                     'policykitagentgui.h', 'policykitagentgui.ui'):
            # Container user IDs can differ from the workspace owner. Trust
            # this specific checkout only for this read-only Git invocation.
            content = subprocess.check_output(
                ['git', '-c', f'safe.directory={ROOT}', '-C', str(ROOT),
                 'show', f'{BASELINE}:src/{path}'], timeout=30)
            (baseline / 'src' / path).write_bytes(content)
        run_case(baseline, 'previous-cancel-does-not-complete',
                 'external_cancel_password', 'f.result.calls == 1')
        run_case(baseline, 'previous-native-callback-blocked',
                 'native_callback_boundary', '!unsafe')
        original = (ROOT / 'src/policykitagent.cpp').read_text()
        ended = 'result.calls == 1 && int(helpers.size()) == starts'
        mutations = [
            ('ignore-cancel', 'choice == QMessageBox::Ok && !m_userCancelled',
             'choice != QMessageBox::NoButton && !m_userCancelled',
             'cancel_retry', ended),
            ('drop-cancel-completion',
             'm_userCancelled = true;\n    finishAuthentication(m_result);',
             'm_userCancelled = true;\n    // Deliberately omit completion.',
             'external_cancel_password', 'f.result.calls == 1'),
            ('allow-stale-answer',
             'if (m_requestId != requestId || m_result != result)\n                return;',
             'if (!m_result)\n                return;', 'stale_dialog',
             'helpers.size() == 2 && next.calls == 0 && f.result.calls == 1'),
            ('allow-finished-callback-to-clear-request',
             'bool PolicykitAgent::initiateAuthenticationFinish()\n{',
             'bool PolicykitAgent::initiateAuthenticationFinish()\n{\n    m_inProgress = false;',
             'finish_callback', ended),
        ]
        for name, before, after, case, expected in mutations:
            if original.count(before) != 1:
                raise RuntimeError(f'Mutation anchor changed: {name}')
            source = root / name
            shutil.copytree(ROOT / 'src', source / 'src')
            (source / 'src/policykitagent.cpp').write_text(original.replace(before, after, 1))
            run_case(source, name, case, expected)


if __name__ == '__main__':
    main()
