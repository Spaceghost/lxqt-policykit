# Native display-server regression tests

These tests run the unmodified production agent sources with real Qt platform
plugins, real window management, and server-delivered input. They are separate
from the offscreen lifecycle suite and remain on the validation branch.

## What is real

- X11 uses the X.Org Xvfb server and Openbox, with Qt's `xcb` platform plugin.
  `xdotool` sends XTEST mouse/keyboard events to the focused client, without
  `--window` event delivery. Alt+F4 goes through Openbox's close binding.
- Wayland uses Sway's headless backend and software rendering, with Qt's
  `wayland` plugin. Xwayland is disabled and DISPLAY is removed. Keyboard input
  arrives via `wtype`'s virtual-keyboard protocol. Sway seat cursor commands
  deliver pointer input through the compositor; Alt+F4 produces a native
  toplevel close request. Those cursor IPC commands are Sway-specific and
  deprecated upstream; their success is checked, not silently ignored.
- Each window must be exposed in Qt and independently present in the server's
  window tree, owned by the expected PID, and focused in both views. Sway
  toplevels must be `xdg_shell`, never Xwayland. The driver does not forcibly
  focus an authentication window to hide focus failures.
- The Qt event filter records spontaneous input. Tests type into and click the
  actual production widgets, and assert that native input arrived. There are
  no QTest input calls, `accept()`, `reject()`, `done()`, or button `click()` calls
  on authentication dialogs in this endpoint or driver.

The observation endpoint reuses the existing deterministic native helper
fixture. Its commands start an authentication request, supply a backend result
or error, simulate caller cancellation, query state, or capture the X root
framebuffer. It cannot perform GUI input. A second process supplies an
independent requesting window and verifies that input returns after completion.

These are real display servers with virtual outputs, not physical-monitor/GPU
tests. PAM, privileged helpers, the system Polkit daemon, and a real editor's
save operation remain outside this fixture. A backend-authorized test result
is not a real credential check. This does not establish that the reviewer's
PAM lockout accounting issue is fixed or cover every compositor/window manager.

## Cases and evidence

Fourteen scenarios run on each server: password cancellation by mouse, Escape,
and window-manager close; retry cancellation by mouse, Escape, window-manager
close, and keyboard traversal; Enter to retry then succeed; three-attempt
exhaustion; backend error acknowledgement before consent; caller cancellation
with each dialog visible; three separate save/cancel requests followed by a
fresh three-attempt request; and selection/retry of the second identity.

They check exact retry text/buttons, modal state, natural focus transitions,
response/helper counts, exactly-once result callbacks, actual native-window
removal, and resumed keyboard input in the independent requesting process.
An explicit negative control verifies that the binary rejects `offscreen`.

Results include per-case logs, JSON state, native window-tree data, server
framebuffer PNGs, server logs, dependency versions, and a JUnit summary. The
workflow builds against `test/production-revision` in a separate worktree and
checks that it remains unchanged. Both server jobs must pass; missing tools,
wrong plugins, server crashes, timeouts, and unexpected dialogs fail the run.

## Running

Install Qt 6 Widgets/Test, PolkitQt6 and Polkit development files, a C++17
compiler, CMake, Python 3, and the applicable server/input tools. X11 needs
Xvfb, xvfb-run, xauth, Openbox, xdotool, xdpyinfo, xprop, and xwininfo. Wayland
needs Sway, wtype, grim, the Qt Wayland plugin, and fonts. A private D-Bus
session is recommended for both.

```sh
cmake -S test/desktop -B /tmp/lxqt-desktop-build \
  -DAGENT_SOURCE_DIR=/path/to/production-checkout
cmake --build /tmp/lxqt-desktop-build --parallel 2
# Run as an ordinary user, not root.
dbus-run-session -- python3 test/desktop/run_desktop.py --backend x11 \
  --binary /tmp/lxqt-desktop-build/desktop_agent --results-dir /tmp/lxqt-x11-results
dbus-run-session -- python3 test/desktop/run_desktop.py --backend wayland \
  --binary /tmp/lxqt-desktop-build/desktop_agent --results-dir /tmp/lxqt-wayland-results
```

The runner creates private servers, ignores inherited display addresses,
uses a private Xauthority cookie or mode-0700 Wayland runtime directory, and
cleans up its own processes. No real desktop, hardware seat, privileged
container, host PAM configuration, or existing login session is needed.
