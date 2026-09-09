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
import shutil
import subprocess
import sys
import tempfile
import time
import traceback
import xml.etree.ElementTree as ET

HERE = Path(__file__).resolve().parent
CASES = (
    'password_mouse_cancel', 'password_escape', 'password_window_close',
    'retry_mouse_cancel', 'retry_escape', 'retry_window_close',
    'retry_keyboard_cancel', 'retry_enter_success', 'retry_limit',
    'backend_error_consent', 'external_cancel_password', 'external_cancel_retry',
    'three_saves_cancel', 'second_identity_retry',
)


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def command(arguments, env, timeout=10):
    result = subprocess.run(arguments, env=env, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, timeout=timeout)
    if result.returncode:
        raise RuntimeError(f'{arguments!r}: exit {result.returncode}: {result.stderr}')
    return result.stdout.strip()


def until(predicate, description, timeout=8):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(0.03)
    raise AssertionError(f'Timed out: {description}')


def stop(process):
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)


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


class Desktop:
    def __init__(self, backend, env, output):
        self.backend = backend
        self.env = env
        self.output = output
        self.processes = []
        self.handles = []

    def spawn(self, args, logfile):
        handle = (self.output / logfile).open('w')
        self.handles.append(handle)
        process = subprocess.Popen(args, env=self.env, stdout=handle, stderr=subprocess.STDOUT)
        self.processes.append(process)
        return process

    def check_alive(self):
        for process in self.processes:
            require(process.poll() is None, f'Display service exited: {process.args!r}')

    def sway(self, *args):
        self.check_alive()
        return json.loads(command(['swaymsg', '-r', '-s', self.env['SWAYSOCK'], *args], self.env))

    def sway_command(self, text):
        response = self.sway(text)
        require(all(item.get('success') for item in response), f'Sway command rejected: {response}')

    def start(self, runtime):
        if self.backend == 'x11':
            config = runtime / 'openbox.xml'
            config.write_text('''<?xml version="1.0"?>
<openbox_config xmlns="http://openbox.org/3.4/rc">
  <focus><focusNew>yes</focusNew><followMouse>no</followMouse></focus>
  <keyboard><keybind key="A-F4"><action name="Close"/></keybind></keyboard>
</openbox_config>
''')
            self.spawn(['openbox', '--sm-disable', '--config', str(config)], 'openbox.log')
            def ready():
                self.check_alive()
                return 'window id #' in command(['xprop', '-root', '_NET_SUPPORTING_WM_CHECK'], self.env)
            until(ready, 'Openbox window-manager registration')
            description = command(['xdpyinfo'], self.env)
            require('XTEST' in description and 'X.Org' in description,
                    'Not an X.Org server with XTEST support')
            (self.output / 'xserver.txt').write_text(description)
        else:
            config = runtime / 'sway.conf'
            config.write_text('''xwayland disable
output HEADLESS-1 mode 1280x800
output * bg #203040 solid_color
seat seat0 fallback true
focus_follows_mouse no
font pango:DejaVu Sans 10
bindsym Mod1+F4 kill
''')
            self.env.update(WLR_BACKENDS='headless', WLR_RENDERER='pixman',
                            WLR_HEADLESS_OUTPUTS='1', WLR_LIBINPUT_NO_DEVICES='1',
                            QT_WAYLAND_DISABLE_WINDOWDECORATION='1')
            compositor = self.spawn(['sway', '--config', str(config), '--debug'], 'sway.log')
            def socket_ready():
                require(compositor.poll() is None, 'Sway failed to start; see sway.log')
                sockets = list(runtime.glob('sway-ipc.*.sock'))
                displays = [p for p in runtime.glob('wayland-*') if p.is_socket()]
                return (sockets[0], displays[0]) if sockets and displays else None
            socket, display = until(socket_ready, 'private Wayland and Sway IPC sockets')
            self.env.update(SWAYSOCK=str(socket), WAYLAND_DISPLAY=display.name)
            # Keep a keyboard on seat0 while individual injection clients come/go.
            self.spawn(['wtype', '-s', '600000'], 'wayland-seat.log')
            until(lambda: self.sway('-t', 'get_seats')[0].get('capabilities', 0) & 2,
                  'Wayland keyboard seat')
            (self.output / 'wayland-server.json').write_text(json.dumps({
                'version': self.sway('-t', 'get_version'),
                'outputs': self.sway('-t', 'get_outputs'),
                'seats': self.sway('-t', 'get_seats'),
            }, indent=2))

    def tree(self):
        return self.sway('-t', 'get_tree')

    @staticmethod
    def nodes(node):
        yield node
        for child in node.get('nodes', []) + node.get('floating_nodes', []):
            yield from Desktop.nodes(child)

    def native_window(self, state, window):
        self.check_alive()
        if self.backend == 'wayland':
            matches = [node for node in self.nodes(self.tree())
                       if node.get('pid') == state['pid'] and node.get('name') == window['title']
                       and node.get('visible')]
            if not matches:
                return None
            require(len(matches) == 1, 'Ambiguous native toplevel')
            node = matches[0]
            require(node.get('shell') == 'xdg_shell' and not node.get('window'),
                    'Wayland test silently used Xwayland')
            rect, content = node['rect'], node['window_rect']
            return dict(id=node['id'], x=rect['x'] + content['x'], y=rect['y'] + content['y'],
                        width=content['width'], height=content['height'], focused=node['focused'])
        info = subprocess.run(['xwininfo', '-id', window['id']], env=self.env,
                              text=True, capture_output=True, timeout=5)
        if info.returncode or 'Map State: IsViewable' not in info.stdout:
            return None
        pid = command(['xdotool', 'getwindowpid', window['id']], self.env)
        require(int(pid) == state['pid'], 'X11 window belongs to the wrong process')
        geometry = dict(line.split('=', 1) for line in command(
            ['xdotool', 'getwindowgeometry', '--shell', window['id']], self.env).splitlines())
        active = subprocess.run(['xdotool', 'getactivewindow'], env=self.env,
                                text=True, capture_output=True, timeout=5)
        return dict(id=int(window['id']), x=int(geometry['X']), y=int(geometry['Y']),
                    width=int(geometry['WIDTH']), height=int(geometry['HEIGHT']),
                    focused=active.returncode == 0 and active.stdout.strip() == window['id'])

    def mapped(self, probe, kind, focused=True):
        def ready():
            state = probe.request()
            wanted = [w for w in state['windows'] if w['kind'] == kind and w['exposed']]
            if len(wanted) != 1:
                return None
            window = wanted[0]
            native = self.native_window(state, window)
            if not native or native['width'] != window['width'] or native['height'] != window['height']:
                return None
            if focused and not (native['focused'] and window['active']):
                return None
            return state, window, native
        return until(ready, f'{kind} must be exposed, mapped and {"focused" if focused else "visible"}')

    def key(self, key):
        if self.backend == 'x11':
            command(['xdotool', 'key', '--clearmodifiers', key], self.env)
        elif key == 'alt+F4':
            command(['wtype', '-M', 'alt', '-k', 'F4', '-m', 'alt'], self.env)
        else:
            command(['wtype', '-k', key], self.env)

    def type_text(self, text):
        if self.backend == 'x11':
            command(['xdotool', 'type', '--clearmodifiers', '--delay', '10', text], self.env)
        else:
            command(['wtype', '-d', '10', text], self.env)

    def click(self, probe, kind, name):
        state, window, native = self.mapped(probe, kind)
        point = window['controls'][name]
        require(point['enabled'], f'{kind}/{name} is disabled')
        x, y = native['x'] + point['x'], native['y'] + point['y']
        if self.backend == 'x11':
            command(['xdotool', 'mousemove', '--sync', str(x), str(y), 'click', '1'], self.env)
        else:
            self.sway_command(f'seat seat0 cursor set {x} {y}')
            self.sway_command('seat seat0 cursor press button1')
            self.sway_command('seat seat0 cursor release button1')
        until(lambda: probe.request()['native_clicks'] > state['native_clicks'],
              'server-delivered spontaneous pointer event')

    def no_windows(self, pid):
        if self.backend == 'wayland':
            return not any(n.get('pid') == pid for n in self.nodes(self.tree()))
        result = subprocess.run(['xdotool', 'search', '--onlyvisible', '--pid', str(pid)],
                                env=self.env, text=True, capture_output=True, timeout=5)
        require(result.returncode in (0, 1), 'X11 window query failed')
        return result.returncode == 1

    def capture(self, probe, directory, label):
        (directory / f'{label}.json').write_text(json.dumps(probe.request(), indent=2))
        if self.backend == 'wayland':
            (directory / f'{label}-tree.json').write_text(json.dumps(self.tree(), indent=2))
            command(['grim', str(directory / f'{label}.png')], self.env)
        else:
            probe.request('capture', path=str(directory / f'{label}.png'))
            (directory / f'{label}-root.txt').write_text(command(
                ['xprop', '-root', '_NET_ACTIVE_WINDOW', '_NET_CLIENT_LIST_STACKING'], self.env))
        require((directory / f'{label}.png').stat().st_size > 1000, 'Empty server framebuffer capture')

    def close(self):
        for process in reversed(self.processes):
            stop(process)
        for handle in self.handles:
            handle.close()


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
        desktop = Desktop(args.backend, env, destination)
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
        result = subprocess.run(['xvfb-run', '-a', '-s', '-screen 0 1280x800x24 -nolisten tcp',
                                 sys.executable, str(Path(__file__).resolve()), *sys.argv[1:], '--inside-x11'],
                                env=env, timeout=500)
        raise SystemExit(result.returncode)
    execute(args)


if __name__ == '__main__':
    main()
