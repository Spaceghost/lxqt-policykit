# Authentication cancellation and lifecycle regression tests

These tests live on `validation/retry-authentication-cancel`. The clean
production commit, `b8b412fcb1696491f7554ceb2d5891c543a76ae0`, does not include
the harness, its Qt Test dependency, this document, or the validation workflow.
CI checks that the production sources and build inputs remain unchanged.

Run from the validation checkout:

```sh
python3 test/test_retry_completion.py
CXX=clang++ python3 test/test_retry_completion.py
python3 test/test_retry_completion.py --sanitize
python3 test/test_retry_completion.py --case '^review_'
python3 test/lifecycle/check_regressions.py
```

The test runner compiles both complete production translation units, their
real headers, the actual .ui file, and generated Qt metaobjects. It uses real
Qt dialogs, PolkitQt Session and AsyncResult, and GObject signal/refcount
behavior, not an extracted completion method or a duplicate listener model.
The review scenarios additionally use Qt Test mouse and keyboard events.

`--qt` remains an accepted compatibility flag; Qt/PolkitQt development
packages are required for every mode. `--source-tree PATH` selects an alternate
complete checkout; `--case REGEX` selects CTest cases. Unknown selections fail.
The review cases also carry the CTest label `pr-review-173`.

## Tests derived from PR #173

The source requirements are the review discussion, not assumptions about the
implementation:

- [stefonarch's cancellation report](https://github.com/lxqt/lxqt-policykit/pull/173#issuecomment-5573115603):
  three separate save-and-cancel operations reportedly lead to an account-lock
  message; the retry dialog also needs a way to stop authentication.
- [tsujan's wording and button request](https://github.com/lxqt/lxqt-policykit/pull/173#issuecomment-5573199181):
  add Cancel and ask "Authentication failed. Trying again?".
- [The PR's contract](https://github.com/lxqt/lxqt-policykit/pull/173): distinguish
  submitted failures from cancellation, bound attempts to three, and prevent
  dialog callbacks from submitting more than once.

| Requirement | Named review cases | Observable assertions |
| --- | --- | --- |
| Repeated save/cancel must not submit a password or launch retries | `review_three_saves_cancel`, `review_three_saves_typed_cancel`, `review_three_saves_escape`, `review_three_saves_close`, `review_three_saves_multi_identity` | Three distinct requests reuse one agent; every result completes once, all helpers retire, no notice remains, and no response is sent. A fourth request gets its full three-attempt budget and succeeds on attempt three. |
| Cancelling a retry must not poison a later save | `review_three_saves_retry_cancel` | One submitted failure followed by Cancel, repeated across three requests, then a fresh three-attempt request. Distinct cookies and results prevent accidental sharing. |
| A real, explicit retry decision with the requested wording | `review_retry_contract`, `review_retry_enter`, `review_second_retry_cancel` | Exact text and OK/Cancel buttons; Cancel stops after the first or second failed attempt; Enter on the default OK starts exactly one replacement helper using the same identity/cookie. |
| A backend error must not silently bypass the retry decision | `review_backend_error_consent` | Acknowledge the backend error, leave the retry question unanswered across an event-loop turn, and confirm no extra helper or result completion before Cancel. |
| Cancellation and a submitted empty password are different actions | `review_empty_submit_is_attempt` | Clicking OK with an empty field sends exactly one empty response, unlike cancelling an empty or populated field. |
| Replaced or obsolete callbacks must not submit again | `review_replaced_prompt_once`, `review_cancelled_password_stays_cancelled` | Replacing a pending prompt leaves one response per click; an old password dialog cannot submit after cancellation and a new request. |
| Do not hide or bypass a backend lockout | `review_backend_lockout_respected` | A supplied terminal backend message remains visible and does not offer or start a retry. This does not model how PAM counts failures. |

The three-save sequence starts at the agent's request boundary. It does not
open or write a privileged file, run a text editor, or model PAM's lockout
counter. Results remain alive throughout the sequence so delayed duplicate
completion is still detected. Every test runs in its own process.

## Coverage and negative controls

There are 46 scenarios: the 32 existing lifecycle cases plus 14 review-derived
cases. The original cases cover cancellation, stale callbacks, native
completion ordering, reentrant result callbacks, multi-identity and
multi-challenge conversations, success, retry exhaustion, shutdown, and
invalid/concurrent requests. They continue to run alongside the review cases.

The negative-control runner builds the reviewed PR at `1d0eb3f` and requires
the review contract test to fail specifically because Cancel is missing. It
also checks two regressions in the earlier cancellation implementation and
11 deliberately broken variants. Six variants target review requirements:
old wording, a missing Cancel button, ignoring Cancel, carrying attempt counts
between requests, submitting a password on Cancel, and accumulating prompt
handlers. All altered sources must compile successfully before testing.

A control counts as rejected only with exit status 1 and its expected runtime
assertion. Review assertions have stable IDs. Build failures, crashes,
timeouts, unknown tests, and unrelated assertions are not successful controls.
Each CTest case has a ten-second timeout; configure, build, and subprocess
steps also have deadlines.

## Dependencies and CI

The validation workflow uses a GitHub-hosted Ubuntu runner with a Fedora 44
container, Qt 6 Widgets/Test, polkit-qt6, liblxqt, CMake, GCC, Clang, and sanitizer
development dependencies. It builds the entire application with its original
minimum dependency versions unchanged and runs the full suite with GCC,
Clang, and ASan/UBSan before checking the negative controls. The workflow runs
only for pushes to the validation branch, or an explicit manual dispatch.

## Boundaries and live validation

The native helper transport and agent registration C functions are interposed
with a deterministic GObject implementation. This exercises the installed
PolkitQt wrapper, but not PAM, a privileged authentication helper, the Polkit
daemon's cancellation protocol, system-bus registration, or a desktop session.
Offscreen Qt input events are not a compositor or visual integration test.
A reported success here comes from the controlled backend, not a real
credential check.

ASan/UBSan instrument the agent and tests, not precompiled distribution
libraries. Leak detection is disabled for this mixed-library harness; passing
sanitizers is not a whole-process leak-free claim. Explicit helper teardown
checks remain enabled in every mode.

To reproduce the report end to end, use a disposable account and isolated
login session with the same PAM configuration as the reviewer. Open a file
requiring authorization, save and cancel three times, then try another save.
Compare before/after helper launches, submitted responses, completion of the
caller's requests, and the backend's failure/lockout records. Also exercise
wrong password then Cancel, retry then success, retry exhaustion, external
cancellation during either dialog, and multiple eligible identities.

Do not reset or weaken PAM policy to make tests pass. Cancelling prevents
further attempts; it does not undo an already-submitted failure. These tests
do not establish that the reviewer's backend account-lockout report is fixed.
