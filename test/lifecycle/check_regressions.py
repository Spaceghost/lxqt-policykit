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
REVIEWED_PR = '1d0eb3feb688318855d4a754f78aa4ea6aa4cd97'


def run_case(source, name, case, expected_assertion, *, review=False):
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
    # Require the intended assertion, not a compiler failure, crash, timeout,
    # unknown test, or an unrelated check. Review IDs remain stable as code moves.
    prefix = r'(?m)^REVIEW ASSERTION: ' if review else r'(?m)^\d+: '
    expected = prefix + re.escape(expected_assertion) + r'$'
    if outcome.returncode != 1 or not re.search(expected, outcome.stdout):
        raise RuntimeError(f'Negative control did not reach its expected assertion: '
                           f'{name}: exit {outcome.returncode}')
    print(f'REJECTED at runtime: {name}', flush=True)


def historical_source(root, name, revision):
    source = root / name
    (source / 'src').mkdir(parents=True)
    # Only known source paths are materialized; do not extract archives.
    for path in ('policykitagent.cpp', 'policykitagent.h', 'policykitagentgui.cpp',
                 'policykitagentgui.h', 'policykitagentgui.ui'):
        # Container user IDs can differ from the workspace owner. Trust only
        # this checkout, for this read-only invocation, not all repositories.
        content = subprocess.check_output(
            ['git', '-c', f'safe.directory={ROOT}', '-C', str(ROOT),
             'show', f'{revision}:src/{path}'], timeout=30)
        (source / 'src' / path).write_bytes(content)
    return source


def main():
    with tempfile.TemporaryDirectory(prefix='lxqt-negative-') as tmp:
        root = Path(tmp)
        baseline = historical_source(root, 'baseline', BASELINE)
        run_case(baseline, 'previous-cancel-does-not-complete',
                 'external_cancel_password', 'f.result.calls == 1')
        run_case(baseline, 'previous-native-callback-blocked',
                 'native_callback_boundary', '!unsafe')
        reviewed = historical_source(root, 'reviewed-pr', REVIEWED_PR)
        run_case(reviewed, 'reviewed-pr-missing-cancel', 'review_retry_contract',
                 'retry_cancel_button', review=True)
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
            ('keep-dead-identity', '        m_gui->removeIdentity(identity);',
             '        // Deliberately retain the dead choice.',
             'dead_identity', 'choices->count() == 1'),
        ]
        # Each mutation corresponds to an observable promise in the PR/review,
        # not to a private state layout invented by the test.
        review_mutations = [
            ('review-old-wording', 'Authentication failed. Trying again?',
             'Authentication failed. Please try again.',
             'review_retry_contract', 'retry_question_text'),
            ('review-no-cancel-button', 'QMessageBox::Ok | QMessageBox::Cancel',
             'QMessageBox::Ok', 'review_retry_contract', 'retry_cancel_button'),
            ('review-ignore-cancel', 'choice == QMessageBox::Ok && !m_userCancelled',
             'choice != QMessageBox::NoButton && !m_userCancelled',
             'review_second_retry_cancel', 'cancel_completes_request'),
            ('review-carry-attempts-between-saves', '    m_authenticationAttempts = 0;',
             '    // Deliberately keep the previous request counter.',
             'review_three_saves_retry_cancel', 'per_request_retry_budget'),
            ('review-count-cancel-as-submission', '    m_userCancelled = true;\n    finishAuthentication(m_result);',
             '    const auto sessions = m_activeSessions;\n'
             '    for (auto *session : sessions)\n        session->setResponse(QString());\n'
             '    m_userCancelled = true;\n    finishAuthentication(m_result);',
             'review_three_saves_cancel', 'cancel_never_submits_password'),
            ('review-accumulate-prompt-handlers',
             '    disconnect(m_gui, &QDialog::finished, session, nullptr);',
             '    // Deliberately accumulate handlers for one click.',
             'review_replaced_prompt_once', 'one_click_one_response'),
        ]
        for is_review, controls in ((False, mutations), (True, review_mutations)):
            for name, before, after, case, expected in controls:
                if original.count(before) != 1:
                    raise RuntimeError(f'Mutation anchor changed: {name}')
                source = root / name
                shutil.copytree(ROOT / 'src', source / 'src')
                (source / 'src/policykitagent.cpp').write_text(original.replace(before, after, 1))
                run_case(source, name, case, expected, review=is_review)


if __name__ == '__main__':
    main()
