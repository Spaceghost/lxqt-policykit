# Authentication retry and cancellation validation

The tests live on `validation/retry-authentication-cancel`, separately from
the single production commit. `test/production-revision` records the exact
production candidate. The workflow checks out that commit in a separate
worktree and compiles the tests against its unmodified production files.

## Run the suite

From this validation checkout, with Qt 6 Widgets/Test, PolkitQt6, native
Polkit development files, a C++17 compiler, CMake and Python 3 installed:

```sh
python3 test/test_retry_completion.py --results-dir /tmp/retry-debug
CXX=clang++ python3 test/test_retry_completion.py
python3 test/test_retry_completion.py --build-type Release
python3 test/test_retry_completion.py --sanitize
python3 test/test_retry_completion.py --case '^review_'
python3 test/lifecycle/check_regressions.py
```

Use `--source-tree PATH` with either runner to select a separate production
checkout. `--results-dir PATH` retains configure/build/test logs, the CTest
inventory, metadata and JUnit results where CTest supports them. Unknown
scenario selections fail. `--qt` is retained only as a compatibility flag;
all modes use actual Qt widgets and the installed PolkitQt wrapper.

The fixture compiles both complete production translation units, their real
headers, the actual UI file and generated metaobjects. It does not extract
functions into a substitute listener. Only agent registration and the native
helper transport are replaced with a deterministic GObject implementation.

## Attempt contract

An attempt is a conversation to which the selected identity submitted at
least one response. Multiple challenges in that conversation are not extra
attempts. An explicitly submitted empty response is still a submission.
Cancellation sends no additional response and never requests another attempt.
An unsuccessful helper that received no response terminates the request;
its backend error is preserved without offering a password retry. Success
reported by the backend remains valid even without a submitted response.

This is the agent's retry policy, not a model of PAM's failure counter. A
failure after a submitted response is not necessarily proof of a bad password;
the backend remains responsible for authentication and lockout decisions.
No error-message parsing or backend-policy resets are involved.

## Review-derived regressions

The original requirements are in [stefonarch's report](https://github.com/lxqt/lxqt-policykit/pull/173#issuecomment-5573115603)
and [tsujan's response](https://github.com/lxqt/lxqt-policykit/pull/173#issuecomment-5573199181).
The review suite covers three separate save/cancel requests using one agent,
Cancel/Escape/window close, typed but unsubmitted input, multiple identities,
a fourth request with its full retry budget, and cancellation after submitted
failures. Every result remains alive to catch delayed duplicate completion.

It checks the exact `Authentication failed. Trying again?` text, standard
OK/Cancel buttons, Enter on OK, no implicit retry after a backend error,
empty submission versus cancellation, replacement of prompt callbacks, stale
password dialogs, and preservation of a supplied terminal backend message.
These 14 cases carry the CTest label `pr-review-173`.

## Lifecycle and contract coverage

There are 59 behavioral scenarios: 32 original lifecycle cases, 14 review
cases, and 13 additional attempt/lifetime cases. The added cases cover:

- Failure before submission, an error before submission, startup failure of
  a replacement helper, and successful completion without a response.
- Cancellation before the first queued prompt and during a later challenge;
  three conversations with three challenges each still allow three attempts.
- An unselected helper failing without consuming the selected identity's
  budget, and deliberately delayed prompts for multiple identities.
- Stale backend acknowledgements and repeated answers to the retry dialog.
- 150 requests through one agent, with explicit destruction checks for both
  native helpers and QObject session wrappers, including hidden dialogs;
  wrapper destruction on shutdown before deferred events are processed.

The transport fixture can hold completion after a submitted response. Error
and native-callback-order tests use this rather than silently treating a
no-response helper failure as a submitted failure. Request, success, failure,
retry exhaustion, reentrancy, shutdown and concurrent-request checks remain.

The C++ fixture, review cases, lifecycle cases and added contract cases are
split into small headers included by one test translation unit. Test blocks
use the surrounding four-space and next-line-brace layout; assertions retain
stable failure IDs for the negative controls.

## Validate the validators

The negative-control runner builds four historical regression cases and 13
intentionally broken variants. This includes the reviewed PR without Cancel,
the earlier cancellation/lifetime defects, and the previous candidate that
retried a failed helper without a submission. Mutations include ignoring
Cancel, stale callbacks, repeated responses, cross-request counters, missing
submission checks and unowned session wrappers.

Each altered source must compile. Only exit 1 with its specific expected
runtime assertion counts as a rejection. Crashes, timeouts, unknown cases,
compiler failures and unrelated assertions are not accepted as evidence.

Sanitizer builds share one CMake instrumentation target between the actual
agent tests and a small enforcement probe. `-fno-sanitize-recover=undefined`
makes undefined behavior fatal. A 60th CTest case requires deliberate signed
overflow and heap-use-after-free to produce the expected diagnostic and exit
1 before reaching a continuation marker. The overflow probe explicitly allows
runtime recovery to verify that the compile-time prohibition is effective.

ASan/UBSan instrument the agent and tests, not precompiled dependencies. Leak
detection is still disabled for the mixed-library suite; explicit ownership
checks are not a claim that the whole process or dependencies are leak-free.

## CI and evidence

Hosted CI builds the complete candidate in Debug and Release, runs GCC Debug,
Clang Debug, GCC Release, and fatal ASan/UBSan validation, then runs all negative
controls. It verifies that testing leaves the separate candidate worktree
unchanged. A manual workflow dispatch may specify a full candidate SHA already
available in the checkout; other inputs are rejected rather than interpreted
as shell commands or branch expressions.

Artifacts retain the source history as a Git bundle, both source revisions,
compiler/dependency versions, full logs, test inventory, per-run metadata and
available JUnit reports. Artifacts expire after 30 days and can be retained
externally before expiry. The workflow has read-only repository permissions
and does not register a real system authentication agent or change PAM policy.

The current Fedora environment is not evidence of a Qt 6.6 minimum-version
build, a real desktop test, or a PAM/Polkit integration test. See
[LIVE_VALIDATION.md](LIVE_VALIDATION.md) for the remaining acceptance work.
