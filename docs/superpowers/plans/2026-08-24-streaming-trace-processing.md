# Streaming Trace Processing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stream-validate four RAR-packaged traces, run exactly one 60-second ns-3 experiment per archive, and write the highest-network-byte 10-minute 1W trace without temporary leftovers.

**Architecture:** A focused Python module opens JSON or RAR inputs as restartable binary streams, validates individual trace objects with `ijson`, selects windows without building a JSON DOM, and streams filtered output atomically. A thin CLI exposes validation, earliest-window slicing, and maximum-load slicing; the real experiment workflow uses one cleanup-trapped temporary directory and a run ledger.

**Tech Stack:** Python 3.12 standard library, `python3-ijson`, `unrar`, `unittest`, ns-3 `llm_sample`, Bash.

**Spec:** `docs/superpowers/specs/2026-08-24-streaming-trace-processing-design.md`

## Global Constraints

- Install system dependencies with `apt` only; never use `pip`.
- Preserve all four user-provided RAR archives unchanged and never stage them implicitly.
- Remove `traces/example_trace.json`.
- Run `llm_sample` exactly once per added archive; never retry a failed run.
- Use a fixed 60-second experiment for each validation run.
- Rank 10-minute windows by contained `uplinkBytes + downlinkBytes`.
- Count only positive-UL-and-DL operations whose complete lifetime fits in the window.
- Keep useful local operations inside selected tasks, preserve metadata, rebase timing, and prune broken dependency IDs.
- Keep memory bounded independently of expanded archive size.
- Put temporary slices, statistics, logs, and the one-run ledger under `/tmp/llm-trace-check.XXXXXX` and remove that directory through an exit trap.
- Write only the final derived trace to `traces/1W_high_load_10m.json`.

---

### Task 1: Streaming input and validation

**Files:**
- Create: `scripts/trace_stream.py`
- Create: `scripts/test_trace_stream.py`

**Interfaces:**
- Consumes: JSON paths, RAR paths containing exactly one JSON member, or a binary stream supplied by a unit test.
- Produces:

```python
class TraceValidationError(ValueError):
    """Trace input cannot be consumed safely by llm_sample."""

@dataclass(frozen=True)
class TraceSummary:
    trace_count: int
    operation_count: int
    network_operation_count: int
    total_network_bytes: int
    earliest_network_start_ms: float | None
    maximum_operation_end_ms: float

```

```text
open_trace_input(path: Path) -> ContextManager[BinaryIO]
iter_trace_items(stream: BinaryIO) -> Iterator[dict[str, Any]]
validate_stream(stream: BinaryIO) -> TraceSummary
validate_path(path: Path) -> TraceSummary
```

- [ ] **Step 1: Install the streaming dependency with apt**

Run:

```bash
sudo apt-get install -y python3-ijson
python3 -c 'import ijson; print(ijson.__version__)'
```

Expected: `python3-ijson` installs from Ubuntu and the import prints a version.

- [ ] **Step 2: Write failing validation and input tests**

Create `scripts/test_trace_stream.py` with an in-memory valid document and malformed variants:

```python
import io
import json
import sys
import tempfile
import unittest
from contextlib import contextmanager
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPT_DIR))

from trace_stream import TraceValidationError, open_trace_input, validate_stream


VALID_DOCUMENT = {
    "metadata": {"source": "fixture", "generator": "unit-test"},
    "traces": [
        {
            "agentId": 7,
            "agentType": "worker",
            "hostId": 17,
            "tasks": [
                {
                    "taskSequence": 3,
                    "operations": [
                        {
                            "opId": 0,
                            "startOffsetMs": 1000.0,
                            "durationMs": 200.5,
                            "uplinkBytes": 80,
                            "downlinkBytes": 40,
                            "depend": [],
                        },
                        {
                            "opId": 1,
                            "startOffsetMs": 1200.5,
                            "durationMs": 10.0,
                            "uplinkBytes": 0,
                            "downlinkBytes": 0,
                            "depend": [0],
                        },
                    ],
                }
            ],
        }
    ]
}


class TraceValidationTest(unittest.TestCase):
    def make_stream(self, document=VALID_DOCUMENT):
        return io.BytesIO(json.dumps(document).encode("utf-8"))

    def test_summarizes_valid_trace(self):
        summary = validate_stream(self.make_stream())
        self.assertEqual(summary.trace_count, 1)
        self.assertEqual(summary.operation_count, 2)
        self.assertEqual(summary.network_operation_count, 1)
        self.assertEqual(summary.total_network_bytes, 120)
        self.assertEqual(summary.earliest_network_start_ms, 1000.0)
        self.assertEqual(summary.maximum_operation_end_ms, 1210.5)

    def test_rejects_missing_duration(self):
        document = json.loads(json.dumps(VALID_DOCUMENT))
        del document["traces"][0]["tasks"][0]["operations"][0]["durationMs"]
        with self.assertRaisesRegex(TraceValidationError, "durationMs"):
            validate_stream(self.make_stream(document))

    def test_regular_input_can_be_reopened(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "trace.json"
            path.write_text(json.dumps(VALID_DOCUMENT), encoding="utf-8")
            with open_trace_input(path) as first:
                first_summary = validate_stream(first)
            with open_trace_input(path) as second:
                second_summary = validate_stream(second)
            self.assertEqual(first_summary, second_summary)
```

All test commands must set `PYTHONDONTWRITEBYTECODE=1`.

- [ ] **Step 3: Run the tests and verify the red state**

Run:

```bash
cd contrib/llm
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest scripts/test_trace_stream.py
```

Expected: FAIL because `scripts/trace_stream.py` does not exist.

- [ ] **Step 4: Implement restartable JSON/RAR input**

Create `scripts/trace_stream.py`. Use this exact process boundary for RAR input:

```python
@contextmanager
def open_trace_input(path: Path) -> Iterator[BinaryIO]:
    if path.suffix.lower() != ".rar":
        with path.open("rb") as stream:
            yield stream
        return

    listing = subprocess.run(
        ["unrar", "lb", str(path)],
        check=True,
        capture_output=True,
        text=True,
    )
    members = [line for line in listing.stdout.splitlines() if line]
    if len(members) != 1 or not members[0].lower().endswith(".json"):
        raise TraceValidationError(f"{path} must contain exactly one JSON member")

    process = subprocess.Popen(
        ["unrar", "p", "-inul", str(path), members[0]],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if process.stdout is None:
        process.kill()
        raise TraceValidationError(f"unrar did not provide a stream for {path}")
    try:
        yield process.stdout
    finally:
        process.stdout.close()
        stderr = process.stderr.read() if process.stderr else b""
        return_code = process.wait()
        if process.stderr:
            process.stderr.close()
        if return_code != 0:
            raise TraceValidationError(
                f"unrar failed for {path}: {stderr.decode('utf-8', errors='replace').strip()}"
            )
```

Wrap `subprocess.CalledProcessError` and missing-file/tool errors as `TraceValidationError` with the input path.

- [ ] **Step 5: Implement strict streaming validation**

Implement item iteration with `ijson.parse()` and
`ijson.common.ObjectBuilder`. Require one root map key named `traces` whose
value is an array. Accept one optional `metadata` object and materialize it
into a caller-supplied root-field dictionary. Reject duplicate or other root
keys. Feed `(event, value)` pairs whose prefix begins with `traces.item` into
one builder. Track nested `start_map`/`start_array` and
`end_map`/`end_array` events; yield `builder.value` when the item depth returns
to zero. Never retain a yielded trace item.

Use this network predicate:

```python
def is_network_operation(operation: dict[str, Any]) -> bool:
    return operation["uplinkBytes"] > 0 and operation["downlinkBytes"] > 0
```

For every trace, require integer `agentId`, string `agentType`, and list `tasks`. For every task, require list `operations`. For every operation, require the four simulator fields and validate them with:

```python
def require_nonnegative_number(value: Any, location: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float, Decimal)):
        raise TraceValidationError(f"{location} must be numeric")
    number = float(value)
    if not math.isfinite(number) or number < 0.0:
        raise TraceValidationError(f"{location} must be finite and non-negative")
    return number


def require_nonnegative_integer(value: Any, location: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        raise TraceValidationError(f"{location} must be a non-negative integer")
    return value
```

Catch `ijson.JSONError` and raise `TraceValidationError` naming the input. `validate_path()` must open a fresh input through `open_trace_input()` and call `validate_stream()`.

- [ ] **Step 6: Run validation tests**

Run:

```bash
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest scripts/test_trace_stream.py
```

Expected: all validation tests PASS and no `scripts/__pycache__` exists.

- [ ] **Step 7: Commit the validation component**

Run:

```bash
git add scripts/trace_stream.py scripts/test_trace_stream.py
git commit -m "llm: Add streaming trace validation"
```

---

### Task 2: Window selection and streaming slice output

**Files:**
- Modify: `scripts/trace_stream.py`
- Modify: `scripts/test_trace_stream.py`

**Interfaces:**
- Consumes: `open_trace_input()`, `iter_trace_items()`, validated operation dictionaries, and a positive window duration.
- Produces:

```python
@dataclass(frozen=True)
class Window:
    start_ms: float
    end_ms: float
    network_bytes: int
    root_fields: dict[str, Any] = field(default_factory=dict, compare=False)

@dataclass(frozen=True)
class SliceSummary:
    trace_count: int
    task_count: int
    operation_count: int
    network_operation_count: int
    network_bytes: int

```

```text
find_first_window(path: Path, window_ms: float) -> Window
find_high_load_window(path: Path, window_ms: float) -> Window
write_window(path: Path, output_path: Path, window: Window) -> SliceSummary
```

- [ ] **Step 1: Add failing first-window and filter tests**

Add these literal test helpers above the test class:

```python
def operation(op_id, start_ms, duration_ms, uplink_bytes, downlink_bytes):
    return {
        "opId": op_id,
        "startOffsetMs": start_ms,
        "durationMs": duration_ms,
        "uplinkBytes": uplink_bytes,
        "downlinkBytes": downlink_bytes,
        "depend": [],
    }


def make_document(operations):
    return {
        "traces": [
            {
                "agentId": 1,
                "agentType": "worker",
                "tasks": [{"taskSequence": 1, "operations": operations}],
            }
        ]
    }


@contextmanager
def json_path(document):
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "trace.json"
        path.write_text(json.dumps(document), encoding="utf-8")
        yield path
```

Extend the fixture with:

```python
WINDOW_DOCUMENT = {
    "metadata": {"source": "window-fixture", "generator": "unit-test"},
    "traces": [
        {
            "agentId": 1,
            "agentType": "worker",
            "metadata": {"keep": True},
            "tasks": [
                {
                    "taskSequence": 1,
                    "arrivalOffsetMs": 100000.0,
                    "operations": [
                        {
                            "opId": 0,
                            "startOffsetMs": 100000.0,
                            "durationMs": 1000.0,
                            "uplinkBytes": 200,
                            "downlinkBytes": 300,
                            "depend": [],
                        },
                        {
                            "opId": 1,
                            "startOffsetMs": 101000.0,
                            "durationMs": 20.0,
                            "uplinkBytes": 0,
                            "downlinkBytes": 0,
                            "depend": [0, 99],
                        },
                        {
                            "opId": 2,
                            "startOffsetMs": 159500.0,
                            "durationMs": 1000.0,
                            "uplinkBytes": 900,
                            "downlinkBytes": 900,
                            "depend": [1],
                        },
                    ],
                }
            ],
        }
    ]
}
```

Add assertions:

```python
def test_writes_first_active_minute_with_metadata_and_dependencies(self):
    with json_path(WINDOW_DOCUMENT) as source, tempfile.TemporaryDirectory() as directory:
        output = Path(directory) / "slice.json"
        window = find_first_window(source, 60000.0)
        summary = write_window(source, output, window)
        sliced = json.loads(output.read_text(encoding="utf-8"))

    self.assertEqual(window.start_ms, 100000.0)
    self.assertEqual(window.end_ms, 160000.0)
    self.assertEqual(summary.network_bytes, 500)
    self.assertEqual(sliced["metadata"],
                     {"source": "window-fixture", "generator": "unit-test"})
    self.assertEqual(sliced["traces"][0]["metadata"], {"keep": True})
    task = sliced["traces"][0]["tasks"][0]
    self.assertEqual(task["arrivalOffsetMs"], 0.0)
    self.assertEqual([op["opId"] for op in task["operations"]], [0, 1])
    self.assertEqual([op["startOffsetMs"] for op in task["operations"]], [0.0, 1000.0])
    self.assertEqual(task["operations"][1]["depend"], [0])

def test_first_window_skips_network_operation_longer_than_window(self):
    document = make_document([
        operation(0, 50000.0, 70000.0, 900, 900),
        operation(1, 100000.0, 1000.0, 200, 300),
    ])
    with json_path(document) as source:
        window = find_first_window(source, 60000.0)
    self.assertEqual(window.start_ms, 100000.0)
```

`opId=2` crosses the 60-second end and must be excluded. Local `opId=1` remains because its task contains selected network `opId=0`.

- [ ] **Step 2: Add a failing exact maximum-load test**

Use a 600000 ms window and three network operations:

```python
def test_finds_exact_maximum_contained_bytes(self):
    document = make_document([
        operation(0, 0.0, 1000.0, 10, 10),
        operation(1, 200000.0, 1000.0, 200, 300),
        operation(2, 700000.0, 1000.0, 300, 400),
        operation(3, 150000.0, 650000.0, 1000, 1000),
    ])
    with json_path(document) as source:
        window = find_high_load_window(source, 600000.0)

    self.assertEqual(window.start_ms, 101000.0)
    self.assertEqual(window.end_ms, 701000.0)
    self.assertEqual(window.network_bytes, 1200)
```

This catches incorrect endpoint ordering, counting a duration larger than the window, and choosing the later edge of an equal-load plateau.

- [ ] **Step 3: Run both tests and verify the red state**

Run:

```bash
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest scripts/test_trace_stream.py
```

Expected: FAIL because `Window`, `find_first_window`, `find_high_load_window`, and `write_window` are absent.

- [ ] **Step 4: Implement earliest and weighted window selection**

`find_first_window()` validates the positive finite duration and performs a
fresh streaming pass. It chooses the minimum start time among network
operations with `durationMs <= window_ms`, rejecting the input if no such
operation exists, and returns:

```python
Window(
    start_ms=earliest_fitting_start_ms,
    end_ms=earliest_fitting_start_ms + window_ms,
    network_bytes=0,
)
```

`find_high_load_window()` performs a fresh stream pass. For each network operation with `durationMs <= window_ms`, add the byte weight over the inclusive feasible interval:

```python
first_start = max(0.0, operation_start + operation_duration - window_ms)
last_start = operation_start
changes.setdefault(first_start, [0, 0])[0] += operation_bytes
changes.setdefault(last_start, [0, 0])[1] += operation_bytes
```

Sweep sorted timestamps by applying additions, comparing against the best load with strict `>`, then applying removals. Strict comparison preserves the earliest start on ties. Reject an empty event map.

- [ ] **Step 5: Implement streaming filtering and atomic output**

For each trace item, build a deep-copied filtered trace. For each task:

```python
contained = [
    operation
    for operation in task["operations"]
    if operation["startOffsetMs"] >= window.start_ms
    and operation["startOffsetMs"] + operation["durationMs"] <= window.end_ms
]
network_operations = [operation for operation in contained if is_network_operation(operation)]
if not network_operations:
    continue

retained_ids = {operation.get("opId") for operation in contained if "opId" in operation}
for operation in contained:
    operation["startOffsetMs"] -= window.start_ms
    if isinstance(operation.get("depend"), list):
        operation["depend"] = [dependency for dependency in operation["depend"]
                               if dependency in retained_ids]
```

If `arrivalOffsetMs` is numeric, subtract the window start. Omit empty tasks and traces. Write incrementally:

```python
output.write('{\n  "traces": [')
for filtered_trace in filtered_traces:
    output.write("," if not first else "")
    output.write("\n    ")
    json.dump(filtered_trace, output, ensure_ascii=False, separators=(",", ":"))
output.write("\n  ]\n}\n")
```

Create the output with `tempfile.mkstemp(dir=output_path.parent)`, close and replace it with `os.replace()` only after a complete write. Delete the sibling temporary file on exceptions. Verify the written network byte total equals `window.network_bytes` for a high-load window; for a first window, return the measured value in `SliceSummary`.

- [ ] **Step 6: Run all window tests**

Run:

```bash
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest scripts/test_trace_stream.py
```

Expected: all tests PASS with no temporary or bytecode files under `scripts/`.

- [ ] **Step 7: Commit window selection**

Run:

```bash
git add scripts/trace_stream.py scripts/test_trace_stream.py
git commit -m "llm: Add streaming load window selection"
```

---

### Task 3: Command-line interface and obsolete trace removal

**Files:**
- Create: `scripts/find_window.py`
- Modify: `scripts/test_trace_stream.py`
- Delete: `traces/example_trace.json`

**Interfaces:**
- Consumes: `validate_path()`, `find_first_window()`, `find_high_load_window()`, and `write_window()` from `trace_stream.py`.
- Produces these commands:

```text
find_window.py validate INPUT
find_window.py slice-first INPUT OUTPUT --window-seconds SECONDS
find_window.py find-window INPUT OUTPUT --window-minutes MINUTES
```

- [ ] **Step 1: Add failing CLI integration tests**

Run the real CLI through `subprocess.run()` against a temporary JSON path:

```python
def test_cli_writes_first_slice(self):
    with json_path(WINDOW_DOCUMENT) as source, tempfile.TemporaryDirectory() as directory:
        output = Path(directory) / "slice.json"
        completed = subprocess.run(
            [sys.executable, str(SCRIPT_DIR / "find_window.py"),
             "slice-first", str(source), str(output), "--window-seconds", "60"],
            check=False,
            capture_output=True,
            text=True,
            env={**os.environ, "PYTHONDONTWRITEBYTECODE": "1"},
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertTrue(output.is_file())
        self.assertIn("window_start_ms=100000.000", completed.stdout)
        self.assertEqual([path.name for path in Path(directory).iterdir()], ["slice.json"])

def test_validate_accepts_stdin(self):
    completed = subprocess.run(
        [sys.executable, str(SCRIPT_DIR / "find_window.py"), "validate", "-"],
        input=json.dumps(VALID_DOCUMENT),
        capture_output=True,
        text=True,
        env={**os.environ, "PYTHONDONTWRITEBYTECODE": "1"},
    )
    self.assertEqual(completed.returncode, 0, completed.stderr)
    self.assertIn("network_bytes=120", completed.stdout)

def test_two_pass_command_rejects_stdin(self):
    completed = subprocess.run(
        [sys.executable, str(SCRIPT_DIR / "find_window.py"),
         "find-window", "-", "unused.json"],
        input=json.dumps(WINDOW_DOCUMENT),
        capture_output=True,
        text=True,
        env={**os.environ, "PYTHONDONTWRITEBYTECODE": "1"},
    )
    self.assertEqual(completed.returncode, 2)
    self.assertIn("requires a reopenable JSON or RAR path", completed.stderr)
```

- [ ] **Step 2: Run CLI tests and verify the red state**

Run:

```bash
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest scripts/test_trace_stream.py
```

Expected: FAIL because `scripts/find_window.py` does not exist.

- [ ] **Step 3: Implement the thin CLI**

Set `sys.dont_write_bytecode = True` before importing `trace_stream`. Build subparsers with `argparse`; `validate` accepts a path or `-`, while the other commands reject `-` through `parser.error()`.

Use these conversions exactly:

```python
window_ms = arguments.window_seconds * 1000.0
window_ms = arguments.window_minutes * 60.0 * 1000.0
```

Print machine-readable summaries in one line:

```python
print(
    f"window_start_ms={window.start_ms:.3f} "
    f"window_end_ms={window.end_ms:.3f} "
    f"network_bytes={summary.network_bytes} "
    f"traces={summary.trace_count} operations={summary.operation_count}"
)
```

Catch `TraceValidationError`, print `error: <message>` to stderr, and return `1`. Let argparse usage errors return `2`.

- [ ] **Step 4: Run unit and CLI tests**

Run:

```bash
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest scripts/test_trace_stream.py
test ! -e scripts/__pycache__
```

Expected: all tests PASS and no bytecode directory exists.

- [ ] **Step 5: Remove the obsolete empty trace**

Delete only `traces/example_trace.json` with `apply_patch`, then verify:

```bash
test ! -e traces/example_trace.json
git diff --check
```

- [ ] **Step 6: Commit the CLI and deletion**

Run:

```bash
git add scripts/find_window.py scripts/test_trace_stream.py traces/example_trace.json
git commit -m "llm: Add streaming trace CLI"
```

---

### Task 4: Static archive validation and one-shot experiments

**Files:**
- Read: `traces/*.rar`
- Create temporarily: `/tmp/llm-trace-check.XXXXXX/*`
- Create after success: `traces/1W_high_load_10m.json`

**Interfaces:**
- Consumes: the three CLI commands from Task 3 and the built `llm_sample` target.
- Produces: four one-minute temporary slices, exactly four run-ledger entries, four temporary stats files, and the final 10-minute JSON; all temporary products are removed when the shell exits.

- [ ] **Step 1: Build before consuming the one allowed runs**

From the ns-3 root, run:

```bash
./ns3 build llm_sample
```

Expected: target builds successfully. This does not execute a trace.

- [ ] **Step 2: Execute static validation, slicing, exactly four runs, and final selection in one shell session**

Run this command once from the ns-3 root. If it fails, do not rerun any `llm_sample` invocation:

```bash
set -euo pipefail

llm_root="$PWD/contrib/llm"
trace_tmp="$(mktemp -d /tmp/llm-trace-check.XXXXXX)"
trap 'rm -rf -- "$trace_tmp"' EXIT

archives=(
  "$llm_root/traces/1W_端侧优先_tw6m_s42_w10000_st1000_mp_window_detailed_trace_w349000-359000.rar"
  "$llm_root/traces/V2_office_campus_250K.yaml_tw1h_s42_mp_window_detailed_trace_w3407500-3412500_modify.rar"
  "$llm_root/traces/office_campus_250K.yaml_tw1h_s42_window_detailed_trace_w2498500-2503500_modify.rar"
  "$llm_root/traces/office_campus_500K.yaml_tw30m_s42_window_detailed_trace_w1793000-1798000_modify.rar"
)

for index in "${!archives[@]}"; do
  archive="${archives[$index]}"
  slice="$trace_tmp/slice-$index.json"
  PYTHONDONTWRITEBYTECODE=1 python3 "$llm_root/scripts/find_window.py" validate "$archive"
  PYTHONDONTWRITEBYTECODE=1 python3 "$llm_root/scripts/find_window.py" \
    slice-first "$archive" "$slice" --window-seconds 60
  PYTHONDONTWRITEBYTECODE=1 python3 "$llm_root/scripts/find_window.py" validate "$slice"
done

ledger="$trace_tmp/run-ledger.txt"
ulimit -c 0
for index in "${!archives[@]}"; do
  archive="${archives[$index]}"
  archive_name="$(basename "$archive")"
  if test -f "$ledger" && grep -Fqx "$archive_name" "$ledger"; then
    echo "refusing duplicate simulation run for $archive_name" >&2
    exit 1
  fi
  printf '%s\n' "$archive_name" >> "$ledger"

  slice="$trace_tmp/slice-$index.json"
  stats="$trace_tmp/stats-$index.json"
  log="$trace_tmp/run-$index.log"
  if ! ./ns3 run "llm_sample $slice 20 $stats 60" >"$log" 2>&1; then
    tail -80 "$log" >&2
    exit 1
  fi
  test -s "$stats"
  printf 'simulation_ok=%s\n' "$archive_name"
done

test "$(wc -l < "$ledger")" -eq 4

PYTHONDONTWRITEBYTECODE=1 python3 "$llm_root/scripts/find_window.py" \
  find-window "${archives[0]}" "$llm_root/traces/1W_high_load_10m.json" \
  --window-minutes 10
PYTHONDONTWRITEBYTECODE=1 python3 "$llm_root/scripts/find_window.py" \
  validate "$llm_root/traces/1W_high_load_10m.json"

printf 'simulation_runs=4\n'
```

Expected: four `simulation_ok=` lines, `simulation_runs=4`, a successful final validation summary, and automatic deletion of the temporary directory.

- [ ] **Step 3: Stop on any failure without retrying**

If Step 2 returns nonzero, record:

```text
- the last archive named by validation output or the run log tail;
- how many simulation_ok lines appeared before failure;
- that remaining archives were not run;
- that no final trace was generated unless all four runs completed.
```

Do not issue a second `./ns3 run` command for an archive already written to the ledger. Continue implementation only if all four runs pass.

---

### Task 5: Final artifact and cleanup verification

**Files:**
- Verify: `traces/1W_high_load_10m.json`
- Verify absent: `traces/example_trace.json`
- Verify unchanged: `traces/*.rar`
- Verify: `scripts/trace_stream.py`, `scripts/find_window.py`, `scripts/test_trace_stream.py`

**Interfaces:**
- Consumes: successful Task 4 output.
- Produces: evidence that the derived trace is valid, bounded to ten minutes, and no experiment leftovers exist.

- [ ] **Step 1: Re-run only non-simulation tests and static checks**

From `contrib/llm`, run:

```bash
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest scripts/test_trace_stream.py
python3 -m py_compile scripts/trace_stream.py scripts/find_window.py
rm -rf -- scripts/__pycache__
git diff --check
```

Expected: tests and compilation pass; the explicit bytecode directory is removed.

- [ ] **Step 2: Verify final trace boundaries and required files**

Run:

```bash
test -s traces/1W_high_load_10m.json
test ! -e traces/example_trace.json
PYTHONDONTWRITEBYTECODE=1 python3 scripts/find_window.py validate traces/1W_high_load_10m.json
test "$(find traces -maxdepth 1 -name '*.rar' -type f | wc -l)" -eq 4
```

Expected: validation reports a maximum end time no greater than `600000.000` ms and all four RAR files remain.

- [ ] **Step 3: Verify no experiment leftovers**

Run:

```bash
test -z "$(find /tmp -maxdepth 1 -type d -name 'llm-trace-check.*' -print -quit)"
test -z "$(find . -type f \( -name 'slice-*.json' -o -name 'stats-*.json' -o -name 'run-*.log' -o -name 'run-ledger.txt' \) -print -quit)"
test ! -e scripts/__pycache__
```

Expected: every command succeeds with no output.

- [ ] **Step 4: Inspect repository state without staging user data**

Run:

```bash
git status --short
git diff --stat HEAD
stat -c '%n %s bytes' traces/1W_high_load_10m.json
```

Expected: the four RAR archives and the derived 10-minute JSON remain user data in the workspace; committed script changes are clean. Do not add the RAR files or derived JSON to Git unless the user explicitly requests it.

- [ ] **Step 5: Report results**

Report the static summary of each archive, the exact number and outcome of simulation runs, selected 10-minute source boundaries and byte total, final output size, removal of `example_trace.json`, and confirmation that temporary experiment files were deleted.
