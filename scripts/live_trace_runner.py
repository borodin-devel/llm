"""Sequential live-matrix orchestration and process-group timeout handling."""

from __future__ import annotations

import os
from pathlib import Path
import signal
import subprocess
import sys
import time

from live_trace_cleanup import OwnedTemporaryRun
from live_trace_common import (
    POLICY, LiveTraceError, build_llm_command, console_text, format_run_failure,
    reject_legacy_console, validate_policy_coverage,
)
from live_trace_schema import load_output_document


TERM_GRACE_SECONDS = 0.2


def _signal_process_group(process, signal_number):
    """Signal the dedicated process group, tolerating an already-empty group."""
    try:
        os.killpg(process.pid, signal_number)
    except ProcessLookupError:
        pass


def _process_group_exists(process):
    """Return whether the dedicated process group still has a member."""
    try:
        os.killpg(process.pid, 0)
    except ProcessLookupError:
        return False
    return True


def _terminate_process_group(process, timeout_output):
    """TERM a timed-out group, then KILL and reap it after a bounded grace."""
    grace_deadline = time.monotonic() + TERM_GRACE_SECONDS
    _signal_process_group(process, signal.SIGTERM)
    communication_timed_out = False
    try:
        stdout, _ = process.communicate(timeout=TERM_GRACE_SECONDS)
    except subprocess.TimeoutExpired as term_error:
        communication_timed_out = True
        stdout = term_error.stdout
    group_exists = _process_group_exists(process)
    while group_exists:
        remaining = grace_deadline - time.monotonic()
        if remaining <= 0:
            break
        time.sleep(min(0.01, remaining))
        group_exists = _process_group_exists(process)
    if group_exists:
        _signal_process_group(process, signal.SIGKILL)
    if communication_timed_out:
        drained_stdout, _ = process.communicate()
        if drained_stdout is not None:
            stdout = drained_stdout
    if stdout is None:
        stdout = timeout_output
    return console_text(stdout)


def _run_captured(process_factory, command, cwd, timeout_seconds, trace_path):
    """Run one captured command in a dedicated, fully-reaped process group."""
    process = process_factory(
        command,
        cwd=cwd,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
        start_new_session=True,
    )
    try:
        stdout, _ = process.communicate(timeout=timeout_seconds)
    except subprocess.TimeoutExpired as error:
        console = _terminate_process_group(process, error.stdout)
        raise LiveTraceError(
            format_run_failure(
                trace_path,
                command,
                "timeout",
                console,
                f"command exceeded {timeout_seconds} seconds",
            )
        ) from error
    return subprocess.CompletedProcess(command, process.returncode, console_text(stdout))


def run_one_trace(
    outer_root,
    trace_path,
    policy,
    *,
    process_factory=subprocess.Popen,
    temporary_parent=Path("/tmp"),
):
    """Validate and execute exactly one live trace, with unconditional cleanup."""
    outer_root = Path(outer_root).resolve()
    trace_path = Path(trace_path).resolve()
    try:
        relative_trace = trace_path.relative_to(outer_root)
    except ValueError as error:
        raise LiveTraceError(f"{trace_path}: trace is outside outer repository") from error
    timeout_seconds = policy["timeout_seconds"]
    validate_command = [
        "python3", "contrib/llm/scripts/find_window.py", "validate", str(relative_trace),
    ]
    validate_result = _run_captured(
        process_factory, validate_command, outer_root, timeout_seconds, trace_path
    )
    if validate_result.returncode != 0:
        raise LiveTraceError(
            format_run_failure(
                trace_path, validate_command, validate_result.returncode,
                validate_result.stdout, "trace validation command failed",
            )
        )

    with OwnedTemporaryRun.create(trace_path.stem, temporary_parent) as run_owner:
        run_directory = run_owner.path
        command = build_llm_command(relative_trace, run_directory, policy)
        started = time.monotonic()
        result = _run_captured(
            process_factory, command, outer_root, timeout_seconds, trace_path
        )
        wall_time_seconds = time.monotonic() - started
        console = result.stdout
        if result.returncode != 0:
            raise LiveTraceError(
                format_run_failure(
                    trace_path, command, result.returncode, console,
                    "llm_sample command failed",
                )
            )
        try:
            reject_legacy_console(console, trace_path)
            output_path = run_directory / "output.json"
            metrics = load_output_document(
                output_path, relative_trace, run_directory, expected_policy=policy
            )
        except LiveTraceError as error:
            raise LiveTraceError(
                format_run_failure(
                    trace_path, command, result.returncode, console, str(error)
                )
            ) from error
        metrics.update({
            "policy": (
                "auto" if policy["mode"] == "auto" else f"fixed-{policy['seconds']:.1f}s"
            ),
            "return_code": result.returncode,
            "wall_time_s": wall_time_seconds,
            "output_bytes": output_path.stat().st_size,
        })
        return metrics


def run_live_matrix():
    """Run every policy trace sequentially exactly once."""
    outer_root = Path(__file__).resolve().parents[3]
    trace_directory = outer_root / "contrib/llm/traces"
    discovered = sorted(trace_directory.glob("*.json"))
    names = validate_policy_coverage(discovered, trace_directory)
    failures = []
    for name in names:
        trace_path = trace_directory / name
        try:
            metrics = run_one_trace(outer_root, trace_path, POLICY[name])
            validation = ",".join(
                f"{key}={str(value).lower()}"
                for key, value in sorted(metrics["validation"].items())
            )
            print(
                f"PASS trace={name} policy={metrics['policy']} "
                f"timeout_seconds={POLICY[name]['timeout_seconds']} "
                f"return_code={metrics['return_code']} wall_time_s={metrics['wall_time_s']:.3f} "
                f"output_bytes={metrics['output_bytes']} window_count={metrics['window_count']} "
                f"ap_inventory_count={metrics['ap_inventory_count']} "
                f"sta_inventory_count={metrics['sta_inventory_count']} validation={validation}",
                flush=True,
            )
        except LiveTraceError as error:
            failures.append(str(error))
            print(str(error), file=sys.stderr, flush=True)
    if failures:
        raise LiveTraceError(f"live matrix failed for {len(failures)} trace(s)")
    return 0
