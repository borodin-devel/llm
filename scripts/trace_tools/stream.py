#!/usr/bin/env python3

"""Bounded-memory parsing and slicing for llm trace documents."""

from __future__ import annotations

import math
import copy
import json
import os
import sqlite3
import subprocess
import tempfile
import threading
from contextlib import contextmanager
from dataclasses import dataclass, field
from decimal import Decimal
from pathlib import Path
from typing import Any, BinaryIO, Callable, ContextManager, Iterator

import ijson
from ijson.common import ObjectBuilder

CPP_INT_MIN = -(2**31)
CPP_INT_MAX = 2**31 - 1
UNRAR_LIST_TIMEOUT_SECONDS = 60
UNRAR_STREAM_TIMEOUT_SECONDS = 3600
UNRAR_STOP_TIMEOUT_SECONDS = 10
UNRAR_ERROR_TAIL_BYTES = 8192


class TraceValidationError(ValueError):
    """Trace input cannot be consumed safely by llm-scenario."""


@dataclass(frozen=True)
class TraceSummary:
    """Static summary of a validated trace document."""

    trace_count: int
    operation_count: int
    network_operation_count: int
    total_network_bytes: int
    earliest_network_start_ms: float | None
    maximum_operation_end_ms: float


@dataclass(frozen=True)
class Window:
    """Absolute source boundaries and contained network load."""

    start_ms: float
    end_ms: float
    network_bytes: int
    root_fields: dict[str, Any] = field(default_factory=dict, compare=False)


@dataclass(frozen=True)
class SliceSummary:
    """Counts written to a filtered trace document."""

    trace_count: int
    task_count: int
    operation_count: int
    network_operation_count: int
    network_bytes: int


class _WindowEventStore:
    """Disk-backed aggregation of weighted inclusive window-start intervals."""

    def __init__(self) -> None:
        temporary_file = tempfile.NamedTemporaryFile(
            prefix="llm-window-events.", suffix=".sqlite3", delete=False
        )
        temporary_file.close()
        self.path = Path(temporary_file.name)
        self._connection = sqlite3.connect(self.path)
        self._connection.execute("PRAGMA journal_mode=OFF")
        self._connection.execute("PRAGMA synchronous=OFF")
        self._connection.execute("PRAGMA temp_store=FILE")
        self._connection.execute(
            "CREATE TABLE events ("
            "timestamp REAL PRIMARY KEY, "
            "additions INTEGER NOT NULL DEFAULT 0, "
            "removals INTEGER NOT NULL DEFAULT 0"
            ") WITHOUT ROWID"
        )

    def __enter__(self) -> _WindowEventStore:
        return self

    def __exit__(self, exc_type, exc_value, traceback) -> None:
        self._connection.close()
        self.path.unlink(missing_ok=True)

    def add_interval(self, first_start_ms: float, last_start_ms: float, weight: int) -> None:
        self._connection.execute(
            "INSERT INTO events(timestamp, additions) VALUES (?, ?) "
            "ON CONFLICT(timestamp) DO UPDATE SET additions = additions + excluded.additions",
            (first_start_ms, weight),
        )
        self._connection.execute(
            "INSERT INTO events(timestamp, removals) VALUES (?, ?) "
            "ON CONFLICT(timestamp) DO UPDATE SET removals = removals + excluded.removals",
            (last_start_ms, weight),
        )

    def find_best(self) -> tuple[float, int]:
        self._connection.commit()
        active_bytes = 0
        best_bytes = -1
        best_start_ms = 0.0
        for timestamp_ms, additions, removals in self._connection.execute(
            "SELECT timestamp, additions, removals FROM events ORDER BY timestamp"
        ):
            active_bytes += additions
            if active_bytes > best_bytes:
                best_bytes = active_bytes
                best_start_ms = timestamp_ms
            active_bytes -= removals
        if best_bytes < 0:
            raise TraceValidationError("no network operation fits in the requested window")
        return best_start_ms, best_bytes


@contextmanager
def open_trace_input(path: Path) -> Iterator[BinaryIO]:
    """Open a JSON path or stream the sole JSON member of a RAR path."""

    path = Path(path)
    if path.suffix.lower() != ".rar":
        try:
            with path.open("rb") as stream:
                yield stream
        except OSError as error:
            raise TraceValidationError(f"cannot open {path}: {error}") from error
        return

    try:
        listing = subprocess.run(
            ["unrar", "lb", str(path)],
            check=True,
            capture_output=True,
            text=True,
            timeout=UNRAR_LIST_TIMEOUT_SECONDS,
        )
    except (OSError, subprocess.CalledProcessError, subprocess.TimeoutExpired) as error:
        raise TraceValidationError(f"cannot list {path}: {error}") from error

    members = [line for line in listing.stdout.splitlines() if line]
    if len(members) != 1 or not members[0].lower().endswith(".json"):
        raise TraceValidationError(f"{path} must contain exactly one JSON member")

    error_stream = tempfile.TemporaryFile()
    try:
        process = subprocess.Popen(
            ["unrar", "p", "-inul", str(path), members[0]],
            stdout=subprocess.PIPE,
            stderr=error_stream,
        )
    except OSError as error:
        error_stream.close()
        raise TraceValidationError(f"cannot stream {path}: {error}") from error

    if process.stdout is None:
        process.kill()
        error_stream.close()
        raise TraceValidationError(f"unrar did not provide a stream for {path}")

    stream_timed_out = threading.Event()

    def kill_stalled_stream() -> None:
        stream_timed_out.set()
        try:
            process.kill()
        except ProcessLookupError:
            pass

    watchdog = threading.Timer(UNRAR_STREAM_TIMEOUT_SECONDS, kill_stalled_stream)
    watchdog.daemon = True
    watchdog.start()
    consumer_failed = False
    try:
        yield process.stdout
    except BaseException:
        consumer_failed = True
        raise
    finally:
        process.stdout.close()
        if consumer_failed and process.poll() is None:
            process.terminate()
        try:
            return_code = process.wait(timeout=UNRAR_STOP_TIMEOUT_SECONDS)
        except subprocess.TimeoutExpired:
            process.kill()
            return_code = process.wait(timeout=UNRAR_STOP_TIMEOUT_SECONDS)
        finally:
            watchdog.cancel()

        error_stream.flush()
        error_stream.seek(0, os.SEEK_END)
        error_size = error_stream.tell()
        error_stream.seek(max(0, error_size - UNRAR_ERROR_TAIL_BYTES))
        error_tail = error_stream.read()
        error_stream.close()

        if stream_timed_out.is_set() and not consumer_failed:
            raise TraceValidationError(
                f"unrar timed out after {UNRAR_STREAM_TIMEOUT_SECONDS}s for {path}"
            )
        if return_code != 0 and not consumer_failed:
            message = error_tail.decode("utf-8", errors="replace").strip()
            raise TraceValidationError(f"unrar failed for {path}: {message}")


def iter_trace_items(
    stream: BinaryIO, root_fields: dict[str, Any] | None = None
) -> Iterator[dict[str, Any]]:
    """Validate the root shape and yield one materialized trace item at a time."""

    saw_root_map = False
    saw_traces_key = False
    saw_metadata_key = False
    saw_traces_array = False
    closed_traces_array = False
    closed_root_map = False
    builder: ObjectBuilder | None = None
    builder_target: str | None = None
    item_depth = 0

    try:
        events = ijson.parse(stream, use_float=True)
        for prefix, event, value in events:
            if builder is not None:
                builder.event(event, value)
                if event in ("start_map", "start_array"):
                    item_depth += 1
                elif event in ("end_map", "end_array"):
                    item_depth -= 1
                    if item_depth == 0:
                        completed_value = builder.value
                        completed_target = builder_target
                        builder = None
                        builder_target = None
                        if completed_target == "trace":
                            if not isinstance(completed_value, dict):
                                raise TraceValidationError(
                                    "document.traces items must be objects"
                                )
                            yield completed_value
                        elif completed_target == "metadata":
                            if not isinstance(completed_value, dict):
                                raise TraceValidationError("document.metadata must be an object")
                            if root_fields is not None:
                                root_fields["metadata"] = completed_value
                continue

            if prefix == "" and event == "start_map":
                if saw_root_map:
                    raise TraceValidationError("document must contain one root object")
                saw_root_map = True
            elif prefix == "" and event == "map_key":
                if value == "traces" and not saw_traces_key:
                    saw_traces_key = True
                elif value == "metadata" and not saw_metadata_key:
                    saw_metadata_key = True
                else:
                    raise TraceValidationError(
                        "document root supports only unique traces and metadata fields"
                    )
            elif prefix == "traces" and event == "start_array":
                saw_traces_array = True
            elif prefix == "traces.item" and event == "start_map":
                builder = ObjectBuilder()
                builder_target = "trace"
                item_depth = 1
                builder.event(event, value)
            elif prefix == "metadata" and event == "start_map":
                builder = ObjectBuilder()
                builder_target = "metadata"
                item_depth = 1
                builder.event(event, value)
            elif prefix == "traces" and event == "end_array":
                closed_traces_array = True
            elif prefix == "" and event == "end_map":
                closed_root_map = True
            elif prefix.startswith("traces.item"):
                raise TraceValidationError("document.traces items must be objects")
            elif prefix.startswith("metadata"):
                raise TraceValidationError("document.metadata must be an object")
    except ijson.JSONError as error:
        raise TraceValidationError(f"invalid JSON: {error}") from error

    if builder is not None:
        raise TraceValidationError("document ended inside a trace item")
    if not saw_root_map or not closed_root_map:
        raise TraceValidationError("document must be a complete root object")
    if not saw_traces_key or not saw_traces_array or not closed_traces_array:
        raise TraceValidationError("document.traces must be an array")


def _require_mapping(value: Any, location: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise TraceValidationError(f"{location} must be an object")
    return value


def _require_list(value: Any, location: str) -> list[Any]:
    if not isinstance(value, list):
        raise TraceValidationError(f"{location} must be an array")
    return value


def _require_nonnegative_number(value: Any, location: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float, Decimal)):
        raise TraceValidationError(f"{location} must be numeric")
    number = float(value)
    if not math.isfinite(number) or number < 0.0:
        raise TraceValidationError(f"{location} must be finite and non-negative")
    return number


def _require_nonnegative_integer(value: Any, location: str) -> int:
    if (
        isinstance(value, bool)
        or not isinstance(value, int)
        or value < 0
        or value > CPP_INT_MAX
    ):
        raise TraceValidationError(
            f"{location} must be an integer in [0, {CPP_INT_MAX}]"
        )
    return value


def is_network_operation(operation: dict[str, Any]) -> bool:
    """Return whether the C++ parser will retain this operation for traffic."""

    return operation["uplinkBytes"] > 0 and operation["downlinkBytes"] > 0


def _summarize_stream(
    stream: BinaryIO,
    operation_observer: Callable[[dict[str, Any], float, float], None] | None = None,
    root_fields: dict[str, Any] | None = None,
) -> TraceSummary:
    """Validate a stream, optionally observing each normalized operation."""

    trace_count = 0
    operation_count = 0
    network_operation_count = 0
    total_network_bytes = 0
    earliest_network_start_ms: float | None = None
    maximum_operation_end_ms = 0.0

    for trace_index, trace_value in enumerate(iter_trace_items(stream, root_fields)):
        trace_location = f"document.traces[{trace_index}]"
        trace = _require_mapping(trace_value, trace_location)
        for field in ("agentId", "agentType", "tasks"):
            if field not in trace:
                raise TraceValidationError(f"{trace_location}.{field} is required")
        if (
            isinstance(trace["agentId"], bool)
            or not isinstance(trace["agentId"], int)
            or trace["agentId"] < CPP_INT_MIN
            or trace["agentId"] > CPP_INT_MAX
        ):
            raise TraceValidationError(
                f"{trace_location}.agentId must be an integer in "
                f"[{CPP_INT_MIN}, {CPP_INT_MAX}]"
            )
        if not isinstance(trace["agentType"], str):
            raise TraceValidationError(f"{trace_location}.agentType must be a string")

        tasks = _require_list(trace["tasks"], f"{trace_location}.tasks")
        for task_index, task_value in enumerate(tasks):
            task_location = f"{trace_location}.tasks[{task_index}]"
            task = _require_mapping(task_value, task_location)
            if "operations" not in task:
                raise TraceValidationError(f"{task_location}.operations is required")
            operations = _require_list(task["operations"], f"{task_location}.operations")

            for operation_index, operation_value in enumerate(operations):
                operation_location = f"{task_location}.operations[{operation_index}]"
                operation = _require_mapping(operation_value, operation_location)
                for field in (
                    "startOffsetMs",
                    "durationMs",
                    "uplinkBytes",
                    "downlinkBytes",
                ):
                    if field not in operation:
                        raise TraceValidationError(f"{operation_location}.{field} is required")

                start_ms = _require_nonnegative_number(
                    operation["startOffsetMs"], f"{operation_location}.startOffsetMs"
                )
                duration_ms = _require_nonnegative_number(
                    operation["durationMs"], f"{operation_location}.durationMs"
                )
                uplink_bytes = _require_nonnegative_integer(
                    operation["uplinkBytes"], f"{operation_location}.uplinkBytes"
                )
                downlink_bytes = _require_nonnegative_integer(
                    operation["downlinkBytes"], f"{operation_location}.downlinkBytes"
                )

                operation_end_ms = start_ms + duration_ms
                if not math.isfinite(operation_end_ms):
                    raise TraceValidationError(
                        f"{operation_location} operation end must be finite"
                    )

                operation_count += 1
                maximum_operation_end_ms = max(maximum_operation_end_ms, operation_end_ms)
                if is_network_operation(operation):
                    network_operation_count += 1
                    total_network_bytes += uplink_bytes + downlink_bytes
                    if earliest_network_start_ms is None:
                        earliest_network_start_ms = start_ms
                    else:
                        earliest_network_start_ms = min(earliest_network_start_ms, start_ms)

                if operation_observer is not None:
                    operation_observer(operation, start_ms, duration_ms)

        trace_count += 1

    return TraceSummary(
        trace_count=trace_count,
        operation_count=operation_count,
        network_operation_count=network_operation_count,
        total_network_bytes=total_network_bytes,
        earliest_network_start_ms=earliest_network_start_ms,
        maximum_operation_end_ms=maximum_operation_end_ms,
    )


def validate_stream(stream: BinaryIO) -> TraceSummary:
    """Validate a complete stream and return aggregate trace properties."""

    return _summarize_stream(stream)


def _require_window_ms(window_ms: float) -> float:
    if not math.isfinite(window_ms) or window_ms <= 0.0:
        raise TraceValidationError("window duration must be positive and finite")
    return window_ms


def find_first_window(path: Path, window_ms: float) -> Window:
    """Find the earliest network operation that fits in the requested window."""

    window_ms = _require_window_ms(window_ms)
    earliest_start_ms: float | None = None
    root_fields: dict[str, Any] = {}

    def observe(operation: dict[str, Any], start_ms: float, duration_ms: float) -> None:
        nonlocal earliest_start_ms
        if is_network_operation(operation) and duration_ms <= window_ms:
            if earliest_start_ms is None:
                earliest_start_ms = start_ms
            else:
                earliest_start_ms = min(earliest_start_ms, start_ms)

    with open_trace_input(path) as stream:
        _summarize_stream(stream, observe, root_fields)

    if earliest_start_ms is None:
        raise TraceValidationError(f"{path}: no network operation fits in the requested window")
    return Window(
        start_ms=earliest_start_ms,
        end_ms=earliest_start_ms + window_ms,
        network_bytes=0,
        root_fields=root_fields,
    )


def find_high_load_window(path: Path, window_ms: float) -> Window:
    """Find the earliest window with maximum fully contained network bytes."""

    window_ms = _require_window_ms(window_ms)
    root_fields: dict[str, Any] = {}

    with _WindowEventStore() as changes:

        def observe(operation: dict[str, Any], start_ms: float, duration_ms: float) -> None:
            if not is_network_operation(operation) or duration_ms > window_ms:
                return
            first_start_ms = max(0.0, start_ms + duration_ms - window_ms)
            last_start_ms = start_ms
            weight = operation["uplinkBytes"] + operation["downlinkBytes"]
            changes.add_interval(first_start_ms, last_start_ms, weight)

        with open_trace_input(path) as stream:
            _summarize_stream(stream, observe, root_fields)

        try:
            best_start_ms, best_bytes = changes.find_best()
        except TraceValidationError as error:
            raise TraceValidationError(f"{path}: {error}") from error

    return Window(
        start_ms=best_start_ms,
        end_ms=best_start_ms + window_ms,
        network_bytes=best_bytes,
        root_fields=root_fields,
    )


def _filter_trace_item(
    trace_value: dict[str, Any], window: Window
) -> tuple[dict[str, Any] | None, SliceSummary]:
    trace = copy.deepcopy(trace_value)
    selected_tasks = []
    task_count = 0
    operation_count = 0
    network_operation_count = 0
    network_bytes = 0

    for task in trace["tasks"]:
        contained_operations = []
        for operation in task["operations"]:
            start_ms = float(operation["startOffsetMs"])
            end_ms = start_ms + float(operation["durationMs"])
            if start_ms >= window.start_ms and end_ms <= window.end_ms:
                contained_operations.append(operation)

        selected_network_operations = [
            operation for operation in contained_operations if is_network_operation(operation)
        ]
        if not selected_network_operations:
            continue

        retained_ids = {
            operation["opId"] for operation in contained_operations if "opId" in operation
        }
        for operation in contained_operations:
            operation["startOffsetMs"] = float(operation["startOffsetMs"]) - window.start_ms
            if isinstance(operation.get("depend"), list):
                operation["depend"] = [
                    dependency
                    for dependency in operation["depend"]
                    if dependency in retained_ids
                ]

        arrival_offset = task.get("arrivalOffsetMs")
        if isinstance(arrival_offset, (int, float, Decimal)) and not isinstance(
            arrival_offset, bool
        ):
            task["arrivalOffsetMs"] = float(arrival_offset) - window.start_ms
        task["operations"] = contained_operations
        selected_tasks.append(task)

        task_count += 1
        operation_count += len(contained_operations)
        network_operation_count += len(selected_network_operations)
        network_bytes += sum(
            operation["uplinkBytes"] + operation["downlinkBytes"]
            for operation in selected_network_operations
        )

    if not selected_tasks:
        return None, SliceSummary(0, 0, 0, 0, 0)

    trace["tasks"] = selected_tasks
    return trace, SliceSummary(
        trace_count=1,
        task_count=task_count,
        operation_count=operation_count,
        network_operation_count=network_operation_count,
        network_bytes=network_bytes,
    )


def write_window(path: Path, output_path: Path, window: Window) -> SliceSummary:
    """Stream a filtered, rebased window to an atomically replaced JSON path."""

    output_path = Path(output_path)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{output_path.name}.", suffix=".tmp", dir=output_path.parent
    )
    totals = SliceSummary(0, 0, 0, 0, 0)

    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as output:
            output.write('{\n  "traces": [')
            first_trace = True
            with open_trace_input(path) as stream:
                for trace_value in iter_trace_items(stream):
                    filtered_trace, summary = _filter_trace_item(trace_value, window)
                    if filtered_trace is None:
                        continue
                    if not first_trace:
                        output.write(",")
                    output.write("\n    ")
                    json.dump(
                        filtered_trace,
                        output,
                        ensure_ascii=False,
                        allow_nan=False,
                        separators=(",", ":"),
                    )
                    first_trace = False
                    totals = SliceSummary(
                        trace_count=totals.trace_count + summary.trace_count,
                        task_count=totals.task_count + summary.task_count,
                        operation_count=totals.operation_count + summary.operation_count,
                        network_operation_count=(
                            totals.network_operation_count + summary.network_operation_count
                        ),
                        network_bytes=totals.network_bytes + summary.network_bytes,
                    )
            output.write("\n  ]")
            for key, value in window.root_fields.items():
                output.write(",\n  ")
                json.dump(key, output, ensure_ascii=False)
                output.write(": ")
                json.dump(
                    value,
                    output,
                    ensure_ascii=False,
                    allow_nan=False,
                    separators=(",", ":"),
                )
            output.write("\n}\n")

        if totals.network_operation_count == 0:
            raise TraceValidationError("selected window contains no network operations")
        if window.network_bytes > 0 and totals.network_bytes != window.network_bytes:
            raise TraceValidationError(
                "selected window byte mismatch: "
                f"expected {window.network_bytes}, wrote {totals.network_bytes}"
            )
        os.replace(temporary_name, output_path)
        return totals
    except BaseException:
        try:
            os.unlink(temporary_name)
        except FileNotFoundError:
            pass
        raise


def validate_path(path: Path) -> TraceSummary:
    """Open and validate one restartable input path."""

    try:
        with open_trace_input(path) as stream:
            return validate_stream(stream)
    except TraceValidationError as error:
        raise TraceValidationError(f"{path}: {error}") from error
