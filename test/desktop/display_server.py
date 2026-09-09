# SPDX-License-Identifier: LGPL-2.1-or-later
"""Private display servers and native input/window inspection for desktop tests."""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import time


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
            self.spawn(['openbox', '--sm-disable', '--config-file', str(config)], 'openbox.log')
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
            # Fedora attaches scheduling capabilities to /usr/bin/sway. Those
            # capabilities can make exec fail inside an ordinary container.
            # Copy bytes only, without xattrs or setuid bits. Headless rendering
            # needs no capabilities; never add SYS_NICE or use privileged mode.
            installed = Path(shutil.which('sway'))
            executable = runtime / 'sway-unprivileged'
            shutil.copyfile(installed, executable)
            executable.chmod(0o755)
            require(hashlib.sha256(installed.read_bytes()).digest()
                    == hashlib.sha256(executable.read_bytes()).digest(), 'Sway copy differs')
            require('security.capability' not in os.listxattr(executable), 'Privileged compositor copy')
            compositor = self.spawn([str(executable), '--config', str(config), '--debug'], 'sway.log')
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
