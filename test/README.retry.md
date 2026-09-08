# Retry-completion regression tests

The runner extracts `PolicykitAgent::completed()` and its attempt limit from
`src/policykitagent.cpp` at test time and compiles that exact method. It does
not maintain a second implementation of the retry decision.

## Run

With Python 3 and a C++17 compiler:

```sh
python3 test/test_retry_completion.py
CXX=clang++ python3 test/test_retry_completion.py
CXXFLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  python3 test/test_retry_completion.py
```

With Qt 6 Widgets development tools (including moc and pkg-config):

```sh
QT_QPA_PLATFORM=offscreen python3 test/test_retry_completion.py --qt
```

`--qt` uses actual QMessageBox buttons, Escape and window-close events, and
actual QPointer destruction tracking. The default mode uses lightweight
control-flow doubles and requires no Qt installation. Set `MOC` if Qt's moc
executable is not in a standard location. Each run has a 30-second runtime
limit so an unexpected modal dialog cannot hang a runner indefinitely.

The workflow in this branch runs both modes on a GitHub-hosted Ubuntu runner.
It is restricted to the separate cancellation branch, not the upstream PR
branch. The unit harness can use Qt 6.4 supplied by Ubuntu 24.04; it does not
lower the application's Qt 6.6 minimum or build the complete application.

## Coverage and boundaries

The 21 scenarios cover Cancel, Escape, close, rejection and unexpected dialog
results; exact text and button set; retry with and without a preceding backend
error; three total attempts; successful completion; already-cancelled and
informational-terminal states; unselected identities; cancellation/terminal
state arriving during the question; replaced sessions; and synchronous
completion callbacks. They assert session-start and result-completion counts.

In both modes, Session, AsyncResult, GUI identity selection, and the surrounding
listener state are test doubles. Tests call the extracted completion method
directly: they do not exercise listener registration, the password dialog's
signals, native Polkit session ownership, PAM, account lockout, or the complete
application. In particular, the stale-session test asserts that an old dialog
cannot touch the replacement request; it does not assert that the full
listener has completed or cleaned up every superseded request.

The default-mode suite was also checked against the original PR implementation
and mutations that ignore Cancel, return before completion, retry after an
external cancellation, remove the session guards, or clear state after a
completion callback. All failed runtime assertions. To check an earlier source:

```sh
git show 1d0eb3feb688318855d4a754f78aa4ea6aa4cd97:src/policykitagent.cpp > /tmp/agent-before.cpp
python3 test/test_retry_completion.py --source /tmp/agent-before.cpp
# Expected failure, not a passing test of the old implementation.
```

## Live verification still required

Build the complete agent normally and use an isolated desktop session and a
disposable account. Do not risk locking the real user's account or change PAM
policy to make the tests pass. Ensure only the agent under test is registered.

Check a denied password followed by Cancel, Escape, and window close: the
original caller must return without authorization, no replacement helper must
start, and no extra generic failure message should appear. Check OK then a
correct password, three denied attempts, a backend error before failure, an
informational terminal condition, cancellation from the original password
form, multiple identities, and a new request immediately after cancellation.

Also cancel the requesting operation while its retry question is open. An OK
answer must not start another helper. This patch does not redesign the existing
cancelAuthentication() override or automatically dismiss all agent windows on
daemon-side cancellation; broader listener/session lifetime behavior needs a
live Polkit test.

Compare the reviewer's repeated save-and-cancel reproduction with the original
PR under the same PAM configuration. Preventing the next retry does not undo
an earlier failed password or establish a fix for backend lockout accounting.
Do not log passwords or authentication cookies during these checks.
