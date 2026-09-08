# Authentication cancellation and lifecycle regression tests

Run from the repository root:

```sh
python3 test/test_retry_completion.py
CXX=clang++ python3 test/test_retry_completion.py
python3 test/test_retry_completion.py --sanitize
python3 test/lifecycle/check_regressions.py
```

The first three commands compile **both complete production translation
units**, their real headers, the actual .ui file, and generated Qt metaobjects.
They use real QMessageBox, password-dialog signals, PolkitQt Session,
PolkitQt AsyncResult, and GObject signal/refcount behavior. They no longer
extract completed() into a duplicate listener/state model. `--qt` remains an
accepted compatibility flag; Qt/PolkitQt development packages are now required
for every mode. `--source-tree PATH` selects an alternate complete checkout;
`--case REGEX` selects CTest cases. Unknown selections fail rather than pass.

## Dependencies and CI

The separate cancellation branch workflow uses a GitHub-hosted Ubuntu runner
with a Fedora 44 container. It installs Qt 6, polkit-qt6, liblxqt, CMake, GCC,
Clang and sanitizer development dependencies, builds the entire application
with its original minimum dependency versions unchanged, and runs this suite.
The container does not register an authentication agent or change host PAM
configuration. The workflow is restricted to retry-authentication-cancel.

## Coverage

Thirty isolated-process scenarios cover the password dialog and retry question
(Cancel, Escape, close, rejection, unexpected result); exact text/buttons;
bounded retries and successful authorization; backend-error acknowledgement
before retry; informational-terminal suppression; externally initiated
cancellation while entering a password or answering a question; native
completion queued during cancellation; stale messages and answers after a
replacement request; later requests and synchronous result callbacks; finish
callbacks that must not reset another request; unrelated identities; cancelling
all identities; multiple challenge prompts; synchronous helper failure;
shutdown; empty identity lists; concurrent requests; and the native callback
boundary. Repeated cancellation runs thirty consecutive requests and checks
that no response was submitted. Tests assert helper creation, cancellation,
response counts, native-object teardown, result completion, and visible UI.

Each CTest case has a ten-second timeout. Python build/configure/run steps also
have deadlines. The negative-control runner compiles the previous cancellation
commit and four intentionally broken variants successfully before requiring
runtime test failures. Build failures are not accepted as regression evidence.

## Boundaries

The native helper transport and agent registration C functions are interposed
with a deterministic GObject test implementation. This exercises the installed
PolkitQt wrapper and its actual signal/lifetime behavior, but does **not** run
PAM, a privileged authentication helper, the Polkit daemon's cancellation
protocol, system-bus registration, or a real desktop session. Offscreen Qt is
not a visual or window-manager integration test. Success in the fixture means
the fake backend reported authorization; it does not bypass real authorization.

ASan/UBSan instrument the full agent and test code, not the distribution's
precompiled Qt/Polkit libraries. Leak detection is disabled for this mixed
library harness; passing sanitizers is not a whole-process leak-free claim.
The helper teardown checks remain enabled in every mode.

## Live validation still required

Use a disposable account and an isolated login session. Exercise wrong password
then Cancel/escape/window-close; retry then successful password; retry exhaustion;
password cancellation; caller cancellation while either dialog is visible;
multiple eligible identities; and a later request after each cancellation.
Check that Cancel launches no further helper and the caller is not left pending.

Reproduce the reviewer's repeated-save-and-cancel report against the same PAM
configuration both before and after this patch. A submitted failure cannot be
undone, and this change does not reset, weaken, or make claims about backend
account-lockout policy. The existing interpretation of showInfo as terminal is
preserved rather than redesigned here.
