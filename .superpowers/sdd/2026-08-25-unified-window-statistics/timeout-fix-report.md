# Process-group timeout follow-up

## Root cause

`_terminate_process_group` sent `SIGKILL` only when the TERM-grace
`communicate()` timed out. If the process-group leader died and a stubborn
grandchild had closed the captured stdout and stderr descriptors,
`communicate()` returned successfully even though that grandchild remained in
the same process group. The function then returned without probing or killing
the surviving group.

## RED

The new real-process regression starts a parent in the runner's dedicated
session. Its grandchild records its PID, ignores `SIGTERM`, redirects stdout
and stderr to `/dev/null`, waits, writes a survival marker, and remains alive.
The test also has bounded failure cleanup so the deliberately reproduced leak
does not persist.

```bash
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest -v \
  test_live_trace_runner.LiveTraceRunnerTest.test_timeout_kills_grandchild_after_leader_closes_pipe
```

Against the original implementation, the test returned promptly but reported
both intended failures:

```text
[survival marker] ... FAIL
AssertionError: True is not false : grandchild survived timeout cleanup
[process state] ... FAIL
AssertionError: 'S' != 'Z' : grandchild remains live after timeout
Ran 1 test in 0.714s
FAILED (failures=2)
```

## GREEN

The minimal fix gives the whole dedicated process group the bounded TERM
grace, polling for group disappearance to reduce process-group reuse risk.
After the grace it probes the group and sends `SIGKILL` if any member remains.
If the first `communicate()` timed out, it then calls `communicate()` again to
drain captured output and reap the leader. Both `SIGTERM` and `SIGKILL` paths
tolerate `ProcessLookupError`.

The new closed-pipe case and the existing inherited-open-pipe case passed
together:

```text
Ran 2 tests in 1.808s
OK
```

Final deterministic verification:

```text
python3 -m py_compile ...                         exit 0
LiveTraceRunnerTest                              9/9 passed
live_test_traces.py --self-test                 40/40 passed
check-style-clang-format.py                     no issues
git diff --check                                no issues
./ns3 build llm-test llm_sample                 exit 0
./test.py -s llm                                1/1 passed
./test.py -e 'llm_sample*'                      1/1 passed
```

The real trace matrix was intentionally not rerun because this follow-up is
limited to runner timeout cleanup. No `llm-trace-live.*` temporary directory
or Python cache remains. The preserved outer artifact still has SHA-256
`bc62df0060a4b7be9b7d4b841c69a64753f34a65f5cdf1df4ddb779ba8703b2a`.

## Fix round 1: identifier reuse

Review identified two check-then-signal reuse hazards. The process-group grace
loop discarded an observed-absent result and probed the same numeric PGID
again before SIGKILL. The regression injects the sequence `False, True` and
requires exactly one probe and only the initial SIGTERM. Against `4c7bfa0`, it
observed two probes and TERM plus KILL.

The test-only failure cleanup also signaled a numeric PID before checking that
it was still the grandchild. New regressions inject a changed `/proc` identity
and an already-absent process; both require zero signal calls. Against
`4c7bfa0`, both paths called SIGKILL, and the mismatch path did not raise.

The focused RED run reported all intended observations:

```text
Ran 3 tests in 0.003s
FAILED (failures=5)
```

The production loop now retains `group_exists` across iterations. Once a
probe reports absence, that PGID is never probed or signaled again. SIGKILL is
sent only when the last observation remains present at the grace deadline.

The test cleanup now captures `(pid, starttime)` from field 22 of
`/proc/<pid>/stat`, re-reads that identity before signaling, treats absence as
success, and raises on mismatch without signaling. Its helper seam injects
identity reads and signals for deterministic safety tests. The post-signal
wait also follows the captured identity instead of numeric PID existence.

Fix-round verification:

```text
python3 -m py_compile ...                         exit 0
LiveTraceRunnerTest                             12/12 passed
live_test_traces.py --self-test                 43/43 passed
check-style-clang-format.py                     no issues
git diff --check                                no issues
./ns3 build llm-test llm_sample                 exit 0
./test.py -s llm                                1/1 passed
./test.py -e 'llm_sample*'                      1/1 passed
```

Both real grandchild cases remain green. The real trace matrix was not rerun.

## Fix round 2: record identity before cleanup races

Review found that the closed-pipe fixture still derived its expected starttime
from `/proc/<pid>/stat` after `_run_captured` returned. A reused PID could
therefore make the unrelated process's current identity become the expected
identity. The inherited-open-pipe fixture recorded only the PID.

The focused regression stores `123 456`, simulates current `/proc` identity
`123 789` after run completion, and requires mismatch refusal with no signal.
The extracted old caller instead adopted `(123,789)` as expected and signaled
that PID:

```text
Ran 1 test in 0.001s
FAILED (failures=2)
```

Both real grandchild programs now read their own `/proc/self/stat` field 22
and write `PID starttime` to an identity file immediately after installing the
TERM handler and before sleeping. Parent tests parse that stored tuple. The
cleanup helper receives only the stored expected tuple and rechecks the
current `/proc` tuple before any signal; it never derives expected identity
after `_run_captured` returns.

Both deterministic reuse coverage and the real open-/closed-pipe cases passed
together. Final verification remained:

```text
LiveTraceRunnerTest                             12/12 passed
live_test_traces.py --self-test                 43/43 passed
./ns3 build llm-test llm_sample                 exit 0
./test.py -s llm                                1/1 passed
./test.py -e 'llm_sample*'                      1/1 passed
```

Syntax, style, and diff checks passed. The real trace matrix was not rerun.
