# Streaming Trace Processing Design

## Goal

Validate the four RAR-packaged traces with one 60-second ns-3 run per
archive, then derive the highest-network-load 10-minute trace from
`1W_端侧优先_tw6m_s42_w10000_st1000_mp_window_detailed_trace_w349000-359000.rar`
as `traces/1W_high_load_10m.json` without leaving extracted files, run
statistics, or logs behind.

## Constraints

- Use `apt` only for system dependencies.
- Preserve all user-provided RAR archives.
- Remove `traces/example_trace.json`.
- Run `llm_sample` exactly once per added archive; do not retry a failed run.
- Each validation run uses a fixed 60-second experiment.
- Rank candidate 10-minute windows by total `uplinkBytes + downlinkBytes`.
- Include only network operations whose complete lifetime fits in the selected
  window when calculating load.
- Preserve trace, task, and operation metadata in generated slices.
- Keep memory bounded independently of expanded archive size.
- Keep temporary slices, statistics, and logs under one cleanup-trapped
  temporary directory.

## Input Characteristics

Each archive contains one JSON object with a top-level `traces` array. Expanded
sizes are approximately 171 MB, 1.95 GB, 3.44 GB, and 6.90 GB. Each trace item
contains `agentId`, `agentType`, and `tasks`; each task contains `operations`.
Network operations have positive `uplinkBytes` and `downlinkBytes`. Local
operations use zero bytes but still carry useful timing and dependency
metadata.

The machine has 15 GB of RAM. Loading the larger JSON documents with Python's
`json` module or ns-3's nlohmann DOM parser is unsafe. Processing must operate
on a stream of individual trace items.

## Dependencies

- `unrar`, installed through `apt`, streams the JSON member with `unrar p`.
- `python3-ijson`, installed through `apt`, incrementally parses
  `traces.item` objects from standard input or a file.
- Python standard-library modules provide CLI parsing, temporary files,
  atomic output replacement, and unit tests.

No Python packages are installed with `pip`.

## Components

### `scripts/trace_stream.py`

This module owns JSON streaming, validation, filtering, and output. It exposes
small functions that accept binary input streams so unit tests can use real
in-memory JSON without mocking the parser.

Responsibilities:

1. Iterate top-level trace items using `ijson.items(stream, "traces.item")`.
2. Validate the fields consumed by `ParseJsonFile()`:
   - integer `agentId`;
   - string `agentType`;
   - array `tasks`;
   - array `operations` per task;
   - finite, non-negative `startOffsetMs` and `durationMs`;
   - non-negative integer `uplinkBytes` and `downlinkBytes`.
3. Report trace count, operation count, network-operation count, total network
   bytes, earliest network start, and maximum operation end.
4. Stream a selected slice to JSON without accumulating the complete output in
   memory.
5. Preserve arbitrary metadata dictionaries on trace, task, and operation
   objects.

The real input root currently contains only `traces`. The writer emits the
same top-level shape. A second large top-level collection is rejected instead
of being silently dropped.

### `scripts/find_window.py`

This is the user-facing CLI. It has three subcommands:

- `validate`: consume a JSON stream and print its static summary;
- `slice-first`: write the first active, fully contained window of a requested
  duration;
- `find-window`: find and write the highest-byte fully contained window.

Inputs may be regular JSON paths or `-` for standard input. Outputs are regular
paths and are written atomically through a sibling temporary file.

### `scripts/test_trace_stream.py`

Unit tests use hand-derived fixtures to cover:

- malformed required fields;
- earliest-active 60-second slicing;
- exact weighted 10-minute window selection;
- exclusion of operations crossing a boundary;
- inclusion of local operations belonging to a selected task;
- timestamp rebasing;
- pruning dependency IDs that refer to removed operations;
- preservation of metadata and input immutability;
- CLI streaming through standard input and atomic output creation.

## Streaming Algorithms

### Full validation

Every archive is streamed to completion before any simulation begins. This
detects malformed JSON and schema/value errors without creating extracted
files. Validation is read-only and does not count as a simulation run.

### One-minute validation slice

The first pass determines the earliest start time of a valid network
operation. The slice is `[earliestStart, earliestStart + 60000 ms]`.

A second stream pass writes trace items containing at least one network
operation whose start and end are both inside the interval. Within each such
task, all operations fully contained in the interval are retained, including
zero-byte local operations. Empty tasks and traces are omitted.

The writer subtracts the window start from `startOffsetMs` and from a numeric
task `arrivalOffsetMs`. Each operation's `depend` list is filtered to IDs of
operations retained in the same task. Other metadata is copied unchanged.

### Highest-load 10-minute window

For each network operation with duration at most 600000 ms, a 10-minute window
starting at `s` fully contains the operation when:

```text
operationEnd - 600000 <= s <= operationStart
```

The operation therefore contributes its byte weight over that interval of
possible window starts. The first streaming pass records weighted add/remove
events at the interval boundaries. Sorting those boundaries and sweeping the
cumulative weight yields the exact maximum total contained bytes. Ties choose
the earliest window start.

The specified 1W archive is the smallest archive (about 171 MB expanded), so
the boundary map is bounded well below the memory required by a JSON DOM. The
second pass writes the selected window using the filtering and rebasing rules
above.

The CLI prints the source window boundaries, retained operation count, agent
count, and total network bytes. The generated document is validated again
through the streaming reader.

## Experiment Workflow

One shell session creates a directory with `mktemp -d` and installs an exit
trap that removes it. For each archive:

1. Stream the archive member through `find_window.py validate`.
2. Stream it again through `find_window.py slice-first --window-seconds 60`,
   writing a temporary JSON slice.
3. Validate the slice.

Only after all four slices pass static validation, build `llm_sample`. Then run
each slice exactly once:

```text
llm_sample <slice.json> 20 <temporary-stats.json> 60
```

Each run writes stdout/stderr to a temporary log. A failed run prints the log
tail and stops the workflow; no run is repeated. A successful run must return
zero and produce non-empty statistics JSON.

If all four runs succeed, stream the specified archive twice to select and
write `traces/1W_high_load_10m.json`, then validate the final file. The cleanup
trap removes all temporary slices, statistics, and logs.

## Failure Handling

- RAR listing/extraction errors stop before simulation.
- Invalid JSON or trace fields identify the archive and exact trace/task/
  operation location.
- An empty 60-second slice is a validation failure.
- A trace with no network operation that fits in 10 minutes cannot produce the
  final output.
- Output uses atomic replacement so an interrupted write cannot leave a
  partial final JSON.
- Simulation failure stops subsequent work and does not trigger a retry.

## Repository Changes

- Add `scripts/trace_stream.py`.
- Add `scripts/find_window.py`.
- Add `scripts/test_trace_stream.py`.
- Add `traces/1W_high_load_10m.json` after all validations pass.
- Remove `traces/example_trace.json`.
- Keep the four RAR archives unchanged.

## Success Criteria

- Unit and CLI tests pass.
- All four complete archive streams pass static validation.
- Exactly four 60-second `llm_sample` invocations occur, one per archive.
- Every invocation exits successfully and writes non-empty temporary stats.
- `traces/1W_high_load_10m.json` passes streaming and C++ parser-compatible
  validation and spans no more than 600000 ms after rebasing.
- No extracted JSON, temporary slice, statistics file, or experiment log
  remains after the workflow.
- `traces/example_trace.json` no longer exists.
