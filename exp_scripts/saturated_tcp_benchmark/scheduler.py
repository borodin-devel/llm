"""Pure CPU and memory admission policy for saturated TCP workers."""

from __future__ import annotations

from collections.abc import Iterable
from dataclasses import dataclass, replace
import os

from .matrix import ExperimentAttempt
from .resources import MemorySnapshot


DEFAULT_MEMORY_RESERVE_PERCENT = 20
MIN_MEMORY_RESERVE_PERCENT = 15
MAX_MEMORY_RESERVE_PERCENT = 50
CPU_RESERVE = 2
CALIBRATION_MARGIN = 1.25


class SchedulerError(ValueError):
    """An invalid resource policy input."""


@dataclass(frozen=True)
class ExecutionPlan:
    """Canonical attempts split into calibration, baseline, and remaining waves."""

    calibration: ExperimentAttempt
    baseline_wave: tuple[ExperimentAttempt, ...]
    remaining_attempts: tuple[ExperimentAttempt, ...]
    canonical_attempts: tuple[ExperimentAttempt, ...]


def _attempt_key(attempt: ExperimentAttempt) -> tuple[int, int]:
    return (
        attempt.configuration.experiment_id,
        attempt.repetition_attempt,
    )


def plan_execution(
    attempts: Iterable[ExperimentAttempt],
    *,
    complete_matrix: bool,
) -> ExecutionPlan:
    """Plan one reused calibration followed by all matching baselines."""
    canonical = tuple(sorted(attempts, key=_attempt_key))
    if not canonical:
        raise SchedulerError("execution plan must contain at least one attempt")
    if any(not isinstance(attempt, ExperimentAttempt) for attempt in canonical):
        raise SchedulerError("execution plan contains an invalid attempt")
    keys = tuple(_attempt_key(attempt) for attempt in canonical)
    if len(set(keys)) != len(keys):
        raise SchedulerError("execution plan contains duplicate attempt keys")

    if complete_matrix:
        matching = [attempt for attempt in canonical if _attempt_key(attempt) == (126, 1)]
        if len(matching) != 1:
            raise SchedulerError(
                "complete execution plan requires experiment 126 attempt 1 calibration"
            )
        calibration = matching[0]
    else:
        first_attempts = [
            attempt for attempt in canonical if attempt.repetition_attempt == 1
        ]
        if not first_attempts:
            raise SchedulerError("execution plan requires a repetition-one calibration")
        calibration = first_attempts[0]

    baseline_wave = tuple(
        attempt
        for attempt in canonical
        if attempt != calibration and attempt.configuration.sta_count_per_bss == 1
    )
    remaining = tuple(
        attempt
        for attempt in canonical
        if attempt != calibration and attempt not in baseline_wave
    )
    return ExecutionPlan(calibration, baseline_wave, remaining, canonical)


def _require_non_negative_integer(value: object, name: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        raise SchedulerError(f"{name} must be a non-negative integer")
    return value


def _validate_memory_reserve_percent(value: object) -> int:
    if (
        isinstance(value, bool)
        or not isinstance(value, int)
        or not MIN_MEMORY_RESERVE_PERCENT <= value <= MAX_MEMORY_RESERVE_PERCENT
    ):
        raise SchedulerError(
            "memory reserve percent must be an integer in "
            f"[{MIN_MEMORY_RESERVE_PERCENT}, {MAX_MEMORY_RESERVE_PERCENT}]"
        )
    return value


def calculate_max_workers(
    logical_cpu_count: int | None,
    *,
    jobs: int = 0,
    sequential_only: bool = False,
) -> int:
    """Return the CPU-limited worker count, with jobs acting only as a cap."""
    requested_jobs = _require_non_negative_integer(jobs, "jobs")
    if sequential_only:
        return 1
    if logical_cpu_count is None:
        logical_cpu_count = os.cpu_count()
    if (
        isinstance(logical_cpu_count, bool)
        or not isinstance(logical_cpu_count, int)
        or logical_cpu_count <= 0
    ):
        logical_cpu_count = 1
    automatic = max(1, logical_cpu_count - CPU_RESERVE)
    if requested_jobs == 0:
        return automatic
    return min(automatic, requested_jobs)


def calculate_worker_peak_estimate(peak_rss_bytes: int) -> int:
    """Return ``ceil(1.25 * peak)`` without floating-point rounding."""
    peak = _require_non_negative_integer(peak_rss_bytes, "peak RSS")
    if peak == 0:
        raise SchedulerError("peak RSS must be positive")
    return (peak * 5 + 3) // 4


def _validate_snapshot(snapshot: MemorySnapshot) -> None:
    if not isinstance(snapshot, MemorySnapshot):
        raise SchedulerError("memory snapshot has an invalid type")
    total = _require_non_negative_integer(snapshot.mem_total_bytes, "MemTotal")
    available = _require_non_negative_integer(
        snapshot.mem_available_bytes, "MemAvailable"
    )
    if total == 0:
        raise SchedulerError("MemTotal must be positive")
    if available > total:
        raise SchedulerError("MemAvailable must not exceed MemTotal")


@dataclass(frozen=True)
class ResourceScheduler:
    """Immutable exact admission policy with a monotonic worker estimate."""

    worker_peak_estimate_bytes: int
    memory_reserve_percent: int = DEFAULT_MEMORY_RESERVE_PERCENT

    def __post_init__(self) -> None:
        estimate = _require_non_negative_integer(
            self.worker_peak_estimate_bytes,
            "worker peak estimate",
        )
        if estimate == 0:
            raise SchedulerError("worker peak estimate must be positive")
        _validate_memory_reserve_percent(self.memory_reserve_percent)

    def target_reserve_bytes(self, snapshot: MemorySnapshot) -> int:
        """Return the ceiling reserve target for one host snapshot."""
        _validate_snapshot(snapshot)
        return (
            snapshot.mem_total_bytes * self.memory_reserve_percent + 99
        ) // 100

    def can_admit(
        self,
        snapshot: MemorySnapshot,
        active_worker_rss_bytes: Iterable[int] = (),
    ) -> bool:
        """Apply the exact predicted-growth admission inequality."""
        _validate_snapshot(snapshot)
        active_rss = tuple(
            _require_non_negative_integer(value, "active worker RSS")
            for value in active_worker_rss_bytes
        )
        predicted_growth = sum(
            max(0, self.worker_peak_estimate_bytes - current_rss)
            for current_rss in active_rss
        )
        remaining = (
            snapshot.mem_available_bytes
            - predicted_growth
            - self.worker_peak_estimate_bytes
        )
        return remaining >= self.target_reserve_bytes(snapshot)

    def observe_peak(self, peak_rss_bytes: int) -> ResourceScheduler:
        """Return a policy whose estimate never decreases after observation."""
        observed_estimate = calculate_worker_peak_estimate(peak_rss_bytes)
        return replace(
            self,
            worker_peak_estimate_bytes=max(
                self.worker_peak_estimate_bytes,
                observed_estimate,
            ),
        )

    def acceptance_floor_breached(self, snapshot: MemorySnapshot) -> bool:
        """Return true only when available RAM is strictly below 15 percent."""
        _validate_snapshot(snapshot)
        return (
            snapshot.mem_available_bytes * 100
            < snapshot.mem_total_bytes * MIN_MEMORY_RESERVE_PERCENT
        )
