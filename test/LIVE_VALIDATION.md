# Live validation before upstream sign-off

Status: not executed by the deterministic regression suite. Record actual
observations and exact revisions here or in the PR; do not convert this
checklist into a claim of completed validation.

## Environment and containment

Use a disposable test account and isolated login session or VM, with recovery
access that does not depend on the account under test. Match the reviewer's
relevant PAM stack and lockout configuration. Record the distribution, session
(X11 or Wayland), editor/save mechanism, Polkit, PolkitQt, Qt and agent versions.
Do not modify a production authentication stack to run these experiments.

Use equivalent fresh accounts or restored isolated snapshots for before/after
comparisons so the first run's lockout state does not contaminate the second.
Do not weaken, reset or bypass the configured policy to make a scenario pass.

## Reproduce the reported sequence

Run the reviewed PR and candidate separately. Open the same protected file,
request a save, cancel authentication, and repeat three times. Then request a
fourth save. Repeat with an empty input field, typed-but-unsubmitted input,
Escape, window close, and one submitted failure followed by retry cancellation.

For each request record when it began and ended, how many helpers started,
whether any responses were submitted, whether any helper or dialog remained,
and the backend's actual failure/lockout records. Record event counts only:
never collect password contents, entered codes, authentication cookies, or
unredacted credential-bearing traces.

Cancellation must launch no replacement helper, submit no further response,
complete the caller's request, and leave the agent ready for a later request.
A backend may account for a cancelled conversation independently of the agent.
If lockout still occurs, identify that boundary and report it separately. The
absence of a submitted response in a fixture does not prove PAM's counter
cannot advance on a particular system.

## Exercise the real integration boundaries

Verify wrong response then retry then success; three submitted failures;
helper startup/transport failure; multiple eligible identities; and multiple
challenges in one conversation. Cancel the requesting application while the
password form, backend message and retry question are each visible. Confirm
there is no hung caller, stale approval, or authentication restarted by an
answer to a retired dialog.

Check X11 and Wayland focus, stacking, keyboard traversal, Escape and close.
The question must remain discoverable instead of hiding behind the caller.
Test long translated messages, scaling and a screen reader as available; the
requested English string stays in tr() and standard buttons remain translated
through Qt. Offscreen input tests do not establish these desktop properties.

## Compatibility and reporting

Build and run against the declared supported dependency floor, including Qt
6.6 and LXQt 2.4 where available. A build with newer Fedora packages is not that
check. Investigate unsuppressed leak reports separately and justify any narrow
dependency suppression with an observed allocation trace; never suppress the
entire agent or all Qt allocations.

The final PR response should identify the tested production SHA, link its
validation revision/run, map both reviewers' concerns to the relevant evidence,
and explain why callback/ownership changes are required for safe cancellation.
Distinguish deterministic, live, and still-unverified results explicitly.
