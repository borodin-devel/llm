#!/usr/bin/env python3

"""Validate large llm traces and write bounded-memory time-window slices."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.dont_write_bytecode = True

from trace_stream import (
    SliceSummary,
    TraceSummary,
    TraceValidationError,
    Window,
    find_first_window,
    find_high_load_window,
    validate_path,
    validate_stream,
    write_window,
)


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    validate_parser = subparsers.add_parser("validate", help="validate one trace input")
    validate_parser.add_argument("input", help="JSON path, RAR path, or - for stdin")

    first_parser = subparsers.add_parser(
        "slice-first", help="write the first active fitting window"
    )
    first_parser.add_argument("input", help="JSON or RAR path")
    first_parser.add_argument("output", type=Path)
    first_parser.add_argument("--window-seconds", type=float, default=60.0)

    load_parser = subparsers.add_parser(
        "find-window", help="write the maximum-network-byte window"
    )
    load_parser.add_argument("input", help="JSON or RAR path")
    load_parser.add_argument("output", type=Path)
    load_parser.add_argument("--window-minutes", type=float, default=10.0)
    return parser


def _print_trace_summary(summary: TraceSummary) -> None:
    earliest = (
        "none"
        if summary.earliest_network_start_ms is None
        else f"{summary.earliest_network_start_ms:.3f}"
    )
    print(
        f"traces={summary.trace_count} operations={summary.operation_count} "
        f"network_operations={summary.network_operation_count} "
        f"network_bytes={summary.total_network_bytes} "
        f"earliest_network_start_ms={earliest} "
        f"maximum_operation_end_ms={summary.maximum_operation_end_ms:.3f}"
    )


def _print_window_summary(window: Window, summary: SliceSummary) -> None:
    print(
        f"window_start_ms={window.start_ms:.3f} window_end_ms={window.end_ms:.3f} "
        f"network_bytes={summary.network_bytes} traces={summary.trace_count} "
        f"tasks={summary.task_count} operations={summary.operation_count} "
        f"network_operations={summary.network_operation_count}"
    )


def main() -> int:
    parser = _build_parser()
    arguments = parser.parse_args()

    try:
        if arguments.command == "validate":
            if arguments.input == "-":
                summary = validate_stream(sys.stdin.buffer)
            else:
                summary = validate_path(Path(arguments.input))
            _print_trace_summary(summary)
            return 0

        if arguments.input == "-":
            parser.error(f"{arguments.command} requires a reopenable JSON or RAR path")

        input_path = Path(arguments.input)
        if arguments.command == "slice-first":
            window = find_first_window(input_path, arguments.window_seconds * 1000.0)
        else:
            window = find_high_load_window(
                input_path, arguments.window_minutes * 60.0 * 1000.0
            )

        summary = write_window(input_path, arguments.output, window)
        _print_window_summary(window, summary)
        return 0
    except TraceValidationError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
