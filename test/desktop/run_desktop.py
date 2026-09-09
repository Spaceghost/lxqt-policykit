#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Exercise production dialogs through private X.Org/Openbox and Sway servers.

Authentication transport is deterministic; GUI actions use XTEST or the
compositor's input path. Never attach to the caller's desktop or use offscreen.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import selectors
import signal
import shutil
import subprocess
import sys
import tempfile
import time
import traceback
import xml.etree.ElementTree as ET

from display_server import Desktop, require, until, stop

CASES = (
    'password_mouse_cancel', 'password_escape', 'password_window_close',
    'retry_mouse_cancel', 'retry_escape', 'retry_window_close',
    'retry_keyboard_cancel', 'retry_enter_success', 'retry_limit',
    'backend_error_consent', 'external_cancel_password', 'external_cancel_retry',
    'three_saves_cancel', 'second_identity_retry',
)


class Probe:
    def __init__(self, binary, env, log, requester=False):
        self.log = log.open('w')
        self.process = subprocess.Popen(
            [str(binary)] + (['--requester'] if requester else []), env=env,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.log)
        self.sequence = 0
        self.pending = b''
        self.selector = selectors.DefaultSelector()
        self.selector.register(self.process.stdout, selectors.EVENT_READ)

    def request(self, op='snapshot', **values):
        self.sequence += 1
        packet = dict(values, op=op, sequence=self.sequence)
        self.process.stdin.write((json.dumps(packet) + '\n').encode())
        self.process.stdin.flush()
        deadline = time.monotonic() + 8
        while b'\n' not in self.pending:
            remaining = deadline - time.monotonic()
            require(remaining > 0 and self.selector.select(remaining),
                    'The Qt client did not answer its observation channel')
            chunk = os.read(self.process.stdout.fileno(), 65536)
            require(chunk, f'Qt client exited unexpectedly: {self.process.poll()}')
            self.pending += chunk
        line, self.pending = self.pending.split(b'\n', 1)
        state = json.loads(line)
        require(state['sequence'] == self.sequence, 'Mismatched observation reply')
        return state

    def close(self):
        if self.process.poll() is None:
            self.process.stdin.close()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                stop(self.process)
        self.selector.close()
        self.process.stdout.close()
        self.log.close()


def question(desktop, probe):
    state, window, native = desktop.mapped(probe, 'message')
    require(window['text'] == 'Authentication failed. Trying again?', 'Wrong retry wording')
    require(set(window['controls']) == {'ok', 'cancel'}, 'Retry must offer exactly OK and Cancel')
    require(window['modal'], 'Retry question must be application-modal')
    return state, window, native


def submit(desktop, probe):
    before, window, _ = desktop.mapped(probe, 'password')
    require(window['input_length'] == 0, 'Password field retained input from a previous challenge')
    require(window['controls']['password']['focused'], 'Password field did not regain keyboard focus')
    desktop.type_text('desktop-test')
    until(lambda: desktop.mapped(probe, 'password')[1]['input_length'] == len('desktop-test'),
          'server-delivered password input')
    require(probe.request()['native_keys'] > before['native_keys'], 'No native keyboard events arrived')
    desktop.key('Return')


def finished(desktop, probe, starts, completions=1):
    def ready():
        state = probe.request()
        return state if state['results'] == [1] * completions and not state['windows'] else None
    state = until(ready, 'authentication request completion and dialog retirement')
    require(len(state['helpers']) == starts, 'Cancellation or completion launched an extra helper')
    require(all(not h['alive'] for h in state['helpers']), 'Native helper survived completion')
    until(lambda: desktop.no_windows(state['pid']), 'native toplevels must actually disappear')
    return state


def run_case(name, desktop, binary, destination):
    directory = destination / name
    directory.mkdir()
    requester = Probe(binary, desktop.env, directory / 'requester.log', requester=True)
    probe = None
    try:
        desktop.mapped(requester, 'requester')
        probe = Probe(binary, desktop.env, directory / 'agent.log')
        require(probe.request()['platform'] == desktop.env['DESKTOP_EXPECTED_QPA'], 'Wrong Qt backend')
        probe.request('begin', multiple=name == 'second_identity_retry')
        desktop.mapped(probe, 'password')
        desktop.capture(probe, directory, 'password')
        if name.startswith('password_'):
            desktop.type_text('not-submitted')
            if name == 'password_mouse_cancel':
                desktop.click(probe, 'password', 'cancel')
            else:
                desktop.key('Escape' if name == 'password_escape' else 'alt+F4')
            state = finished(desktop, probe, 1)
            require(state['helpers'][0]['responses'] == 0, 'Cancelling submitted a password')
        elif name == 'external_cancel_password':
            probe.request('cancel_request')
            state = finished(desktop, probe, 1)
            require(state['helpers'][0]['responses'] == 0, 'External cancel submitted a response')
        elif name == 'three_saves_cancel':
            for save in range(3):
                if save:
                    probe.request('begin')
                    desktop.mapped(probe, 'password')
                desktop.click(probe, 'password', 'cancel')
                state = finished(desktop, probe, save + 1, save + 1)
                require(all(h['responses'] == 0 for h in state['helpers']), 'Cancel submitted input')
            probe.request('begin')
            for attempt in range(3):
                probe.request('backend', index=3 + attempt, gain=attempt == 2)
                submit(desktop, probe)
                if attempt < 2:
                    question(desktop, probe)
                    desktop.key('Return')
                    desktop.mapped(probe, 'password')
            finished(desktop, probe, 6, 4)
        elif name == 'second_identity_retry':
            desktop.click(probe, 'password', 'identity')
            desktop.key('End')
            desktop.key('Return')
            # Give the editable challenge focus by clicking it through the server.
            desktop.click(probe, 'password', 'password')
            require(desktop.mapped(probe, 'password')[1]['identity'] == 'unix-user:2000',
                    'Server input did not select the second identity')
            submit(desktop, probe)
            question(desktop, probe)
            desktop.key('Return')
            desktop.mapped(probe, 'password')
            probe.request('backend', index=2, gain=True)
            submit(desktop, probe)
            state = finished(desktop, probe, 3)
            require(state['helpers'][0]['responses'] == 0 and state['helpers'][2]['identity'] == 'unix-user:2000',
                    'Retry answered the wrong identity')
        else:
            if name == 'backend_error_consent':
                probe.request('backend', index=0, automatic=False)
            submit(desktop, probe)
            if name == 'backend_error_consent':
                until(lambda: probe.request()['helpers'][0]['responses'] == 1, 'submitted response')
                probe.request('backend', index=0, error='Backend rejected the submitted response', complete=True)
                require(desktop.mapped(probe, 'message')[1]['text'] == 'Backend rejected the submitted response',
                        'Backend explanation was lost')
                desktop.key('Return')
            question(desktop, probe)
            desktop.capture(probe, directory, 'retry')
            # Do not activate/focus the dialog from the driver to make this pass.
            # It must acquire focus naturally, with no retries while unanswered.
            time.sleep(0.1)
            state = probe.request()
            require(len(state['helpers']) == 1 and state['results'] == [0], 'Implicit retry without consent')
            if name in ('retry_mouse_cancel', 'backend_error_consent'):
                desktop.click(probe, 'message', 'cancel')
                finished(desktop, probe, 1)
            elif name in ('retry_escape', 'retry_window_close'):
                desktop.key('Escape' if name == 'retry_escape' else 'alt+F4')
                finished(desktop, probe, 1)
            elif name == 'retry_keyboard_cancel':
                desktop.key('Tab')
                until(lambda: desktop.mapped(probe, 'message')[1]['controls']['cancel']['focused'],
                      'Tab must select Cancel')
                desktop.key('space')
                finished(desktop, probe, 1)
            elif name == 'external_cancel_retry':
                probe.request('cancel_request')
                finished(desktop, probe, 1)
            elif name == 'retry_enter_success':
                desktop.key('Return')
                desktop.mapped(probe, 'password')
                require(len(probe.request()['helpers']) == 2, 'Enter did not start exactly one retry')
                probe.request('backend', index=1, gain=True)
                submit(desktop, probe)
                finished(desktop, probe, 2)
            elif name == 'retry_limit':
                for _ in range(2):
                    question(desktop, probe)
                    desktop.key('Return')
                    submit(desktop, probe)
                window = desktop.mapped(probe, 'message')[1]
                require(set(window['controls']) == {'ok'} and window['text'] == 'Authentication failed',
                        'Third failure offered a fourth attempt')
                desktop.key('Return')
                finished(desktop, probe, 3)
            else:
                raise AssertionError(f'Unimplemented desktop scenario: {name}')
        # With the agent gone, the independent application must be usable again.
        desktop.mapped(requester, 'requester')
        desktop.type_text('resumed')
        until(lambda: requester.request()['windows'][0]['input_length'] == len('resumed'),
              'input must return to the requesting application')
        desktop.capture(probe, directory, 'finished')
        (directory / 'requester-final.json').write_text(json.dumps(requester.request(), indent=2))
    except BaseException:
        if probe and probe.process.poll() is None:
            try:
                desktop.capture(probe, directory, 'failure')
            except Exception:
                pass
        raise
    finally:
        if probe:
            probe.close()
        requester.close()


def execute(args):
    require(os.geteuid() != 0, 'Run desktop tests as an unprivileged user, not root')
    required = ['openbox', 'xdotool', 'xprop', 'xwininfo', 'xdpyinfo'] if args.backend == 'x11' else ['sway', 'swaymsg', 'wtype', 'grim']
    for program in required:
        require(shutil.which(program), f'Missing required desktop test dependency: {program}')
    destination = args.results_dir.resolve()
    destination.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='lxqt-desktop-') as tmp:
        runtime = Path(tmp)
        env = dict(os.environ)
        for key in ('WAYLAND_DISPLAY', 'WAYLAND_SOCKET', 'SWAYSOCK', 'I3SOCK', 'QT_QPA_PLATFORMTHEME', 'QT_STYLE_OVERRIDE', 'QT_PLUGIN_PATH'):
            env.pop(key, None)
        if args.backend == 'wayland':
            env.pop('DISPLAY', None)
            env.pop('XAUTHORITY', None)
        env.update(XDG_RUNTIME_DIR=str(runtime), QT_QPA_PLATFORM='xcb' if args.backend == 'x11' else 'wayland',
                   DESKTOP_EXPECTED_QPA='xcb' if args.backend == 'x11' else 'wayland',
                   QT_ACCESSIBILITY='0', LC_ALL='C.UTF-8')
        desktop = Desktop(args.backend, env, destination, args.binary.with_name('desktop_pointer'))
        outcomes = []
        try:
            desktop.start(runtime)
            # A deliberate offscreen run must be rejected, not pass as a desktop test.
            negative = subprocess.run([str(args.binary)], env=dict(env, QT_QPA_PLATFORM='offscreen'),
                                      input=b'', capture_output=True, timeout=10)
            require(negative.returncode == 2 and b'DESKTOP ASSERTION: real_platform_required' in negative.stderr,
                    'Offscreen backend was not rejected by the desktop probe')
            (destination / 'offscreen-negative-control.log').write_bytes(negative.stderr)
            for name in CASES:
                start = time.monotonic()
                error = None
                try:
                    run_case(name, desktop, args.binary, destination)
                except Exception:
                    error = traceback.format_exc()
                outcomes.append(dict(name=name, seconds=time.monotonic() - start, error=error))
                print(f'{"FAIL" if error else "PASS"}: {args.backend}/{name}', flush=True)
                if error:
                    print(error, flush=True)
        except Exception:
            outcomes.append(dict(name='display_setup', seconds=0, error=traceback.format_exc()))
            raise
        finally:
            desktop.close()
            (destination / 'results.json').write_text(json.dumps(outcomes, indent=2))
            suite = ET.Element('testsuite', name=f'desktop-{args.backend}', tests=str(len(outcomes)),
                              failures=str(sum(bool(o['error']) for o in outcomes)))
            for outcome in outcomes:
                case = ET.SubElement(suite, 'testcase', name=outcome['name'], time=str(outcome['seconds']))
                if outcome['error']:
                    ET.SubElement(case, 'failure').text = outcome['error']
            ET.ElementTree(suite).write(destination / 'results.xml', encoding='utf-8', xml_declaration=True)
        require(len(outcomes) == len(CASES) and not any(o['error'] for o in outcomes), 'Desktop scenarios failed')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--backend', required=True, choices=('x11', 'wayland'))
    parser.add_argument('--binary', required=True, type=Path)
    parser.add_argument('--results-dir', required=True, type=Path)
    parser.add_argument('--inside-x11', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    args.binary = args.binary.resolve(strict=True)
    if args.backend == 'x11' and not args.inside_x11:
        require(shutil.which('xvfb-run'), 'xvfb-run and xauth are required')
        # xvfb-run chooses a free display, creates a private Xauthority cookie,
        # and removes the server and cookie on exit. No -ac or TCP listener.
        env = dict(os.environ)
        for key in ('DISPLAY', 'XAUTHORITY', 'WAYLAND_DISPLAY', 'WAYLAND_SOCKET'):
            env.pop(key, None)
        process = subprocess.Popen(['xvfb-run', '-a', '-s', '-screen 0 1280x800x24 -nolisten tcp',
                                    sys.executable, str(Path(__file__).resolve()), *sys.argv[1:], '--inside-x11'],
                                   env=env, start_new_session=True)
        try:
            raise SystemExit(process.wait(timeout=500))
        finally:
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGTERM)
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait(timeout=5)
    execute(args)


if __name__ == '__main__':
    def terminated(signum, _frame):
        raise SystemExit(128 + signum)
    signal.signal(signal.SIGTERM, terminated)
    main()
