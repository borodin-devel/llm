#!/usr/bin/env python3

"""Bounded-memory parsing and slicing for llm trace documents."""

from __future__ import annotations

import math
import subprocess
from contextlib import contextmanager
from dataclasses import dataclass
from decimal import Decimal
from pathlib import Path
from typing import Any, BinaryIO, ContextManager, Iterator

import ijson
from ijson.common import ObjectBuilder


class TraceValidationError(ValueError):
    """Trace input cannot be consumed safely by llm_sample."""


@dataclass(frozen=True)
class TraceSummary:
    """Static summary of a validated trace document."""

    trace_count: int
    operation_count: int
    network_operation_count: int
    total_network_bytes: int
    earliest_network_start_ms: float | None
    maximum_operation_end_ms: float


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
        )
    except (OSError, subprocess.CalledProcessError) as error:
        raise TraceValidationError(f"cannot list {path}: {error}") from error

    members = [line for line in listing.stdout.splitlines() if line]
    if len(members) != 1 or not members[0].lower().endswith(".json"):
        raise TraceValidationError(f"{path} must contain exactly one JSON member")

    try:
        process = subprocess.Popen(
            ["unrar", "p", "-inul", str(path), members[0]],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
    except OSError as error:
        raise TraceValidationError(f"cannot stream {path}: {error}") from error

    if process.stdout is None:
        process.kill()
        raise TraceValidationError(f"unrar did not provide a stream for {path}")

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
        stderr = process.stderr.read() if process.stderr else b""
        return_code = process.wait()
        if process.stderr:
            process.stderr.close()
        if return_code != 0 and not consumer_failed:
            message = stderr.decode("utf-8", errors="replace").strip()
            raise TraceValidationError(f"unrar failed for {path}: {message}")


def iter_trace_items(stream: BinaryIO) -> Iterator[dict[str, Any]]:
    """Validate the root shape and yield one materialized trace item at a time."""

    saw_root_map = False
    saw_traces_key = False
    saw_traces_array = False
    closed_traces_array = False
    closed_root_map = False
    builder: ObjectBuilder | None = None
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
                        trace = builder.value
                        builder = None
                        if not isinstance(trace, dict):
                            raise TraceValidationError("document.traces items must be objects")
                        yield trace
                continue

            if prefix == "" and event == "start_map":
                if saw_root_map:
                    raise TraceValidationError("document must contain one root object")
                saw_root_map = True
            elif prefix == "" and event == "map_key":
                if value != "traces" or saw_traces_key:
                    raise TraceValidationError("document root must contain only the traces field")
                saw_traces_key = True
            elif prefix == "traces" and event == "start_array":
                saw_traces_array = True
            elif prefix == "traces.item" and event == "start_map":
                builder = ObjectBuilder()
                item_depth = 1
                builder.event(event, value)
            elif prefix == "traces" and event == "end_array":
                closed_traces_array = True
            elif prefix == "" and event == "end_map":
                closed_root_map = True
            elif prefix.startswith("traces.item"):
                raise TraceValidationError("document.traces items must be objects")
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
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        raise TraceValidationError(f"{location} must be a non-negative integer")
    return value


def is_network_operation(operation: dict[str, Any]) -> bool:
    """Return whether the C++ parser will retain this operation for traffic."""

    return operation["uplinkBytes"] > 0 and operation["downlinkBytes"] > 0


def validate_stream(stream: BinaryIO) -> TraceSummary:
    """Validate a complete stream and return aggregate trace properties."""

    trace_count = 0
    operation_count = 0
    network_operation_count = 0
    total_network_bytes = 0
    earliest_network_start_ms: float | None = None
    maximum_operation_end_ms = 0.0

    for trace_index, trace_value in enumerate(iter_trace_items(stream)):
        trace_location = f"document.traces[{trace_index}]"
        trace = _require_mapping(trace_value, trace_location)
        for field in ("agentId", "agentType", "tasks"):
            if field not in trace:
                raise TraceValidationError(f"{trace_location}.{field} is required")
        if isinstance(trace["agentId"], bool) or not isinstance(trace["agentId"], int):
            raise TraceValidationError(f"{trace_location}.agentId must be an integer")
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

                operation_count += 1
                maximum_operation_end_ms = max(
                    maximum_operation_end_ms, start_ms + duration_ms
                )
                if is_network_operation(operation):
                    network_operation_count += 1
                    total_network_bytes += uplink_bytes + downlink_bytes
                    if earliest_network_start_ms is None:
                        earliest_network_start_ms = start_ms
                    else:
                        earliest_network_start_ms = min(earliest_network_start_ms, start_ms)

        trace_count += 1

    return TraceSummary(
        trace_count=trace_count,
        operation_count=operation_count,
        network_operation_count=network_operation_count,
        total_network_bytes=total_network_bytes,
        earliest_network_start_ms=earliest_network_start_ms,
        maximum_operation_end_ms=maximum_operation_end_ms,
    )


def validate_path(path: Path) -> TraceSummary:
    """Open and validate one restartable input path."""

    try:
        with open_trace_input(path) as stream:
            return validate_stream(stream)
    except TraceValidationError as error:
        raise TraceValidationError(f"{path}: {error}") from error
