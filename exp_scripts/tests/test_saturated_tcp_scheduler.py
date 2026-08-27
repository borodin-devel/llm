"""Pure resource-admission policy tests for the saturated TCP runner."""

from __future__ import annotations

import unittest

from saturated_tcp_benchmark.resources import MemorySnapshot
from saturated_tcp_benchmark.matrix import build_matrix, iter_experiment_attempts
from saturated_tcp_benchmark.scheduler import (
    CALIBRATION_MARGIN,
    DEFAULT_MEMORY_RESERVE_PERCENT,
    MAX_MEMORY_RESERVE_PERCENT,
    MIN_MEMORY_RESERVE_PERCENT,
    ResourceScheduler,
    SchedulerError,
    calculate_max_workers,
    calculate_worker_peak_estimate,
    plan_execution,
)


class SaturatedTcpSchedulerTest(unittest.TestCase):
    """Protect literal CPU, memory, and fallback scheduling decisions."""

    def test_defaults_and_cli_boundaries_are_exact(self) -> None:
        self.assertEqual(DEFAULT_MEMORY_RESERVE_PERCENT, 20)
        self.assertEqual(MIN_MEMORY_RESERVE_PERCENT, 15)
        self.assertEqual(MAX_MEMORY_RESERVE_PERCENT, 50)
        self.assertEqual(CALIBRATION_MARGIN, 1.25)

        for accepted in (15, 20, 50):
            with self.subTest(accepted=accepted):
                scheduler = ResourceScheduler(1_000, accepted)
                self.assertEqual(scheduler.memory_reserve_percent, accepted)
        for rejected in (14, 51, True, 20.0):
            with self.subTest(rejected=rejected):
                with self.assertRaises(SchedulerError):
                    ResourceScheduler(1_000, rejected)

    def test_cpu_reserve_and_user_jobs_cap_only(self) -> None:
        self.assertEqual(calculate_max_workers(16, jobs=0), 14)
        self.assertEqual(calculate_max_workers(16, jobs=5), 5)
        self.assertEqual(calculate_max_workers(4, jobs=99), 2)
        self.assertEqual(calculate_max_workers(2, jobs=0), 1)
        self.assertEqual(calculate_max_workers(1, jobs=0), 1)
        with self.assertRaises(SchedulerError):
            calculate_max_workers(8, jobs=-1)

    def test_exact_admission_inequality_reserves_predicted_active_growth(self) -> None:
        scheduler = ResourceScheduler(
            worker_peak_estimate_bytes=1_000,
            memory_reserve_percent=20,
        )
        total = 10_000

        equality = MemorySnapshot(total, 3_500)
        self.assertTrue(
            scheduler.can_admit(equality, active_worker_rss_bytes=(500, 1_000))
        )
        below = MemorySnapshot(total, 3_499)
        self.assertFalse(
            scheduler.can_admit(below, active_worker_rss_bytes=(500, 1_000))
        )

        without_growth_reservation = MemorySnapshot(total, 3_000)
        self.assertFalse(
            scheduler.can_admit(
                without_growth_reservation,
                active_worker_rss_bytes=(500, 1_000),
            )
        )

    def test_memory_pause_resumes_when_a_literal_snapshot_recovers(self) -> None:
        scheduler = ResourceScheduler(2_000, 20)
        active = (1_500,)
        self.assertFalse(
            scheduler.can_admit(MemorySnapshot(10_000, 4_499), active)
        )
        self.assertTrue(
            scheduler.can_admit(MemorySnapshot(10_000, 4_500), active)
        )

    def test_peak_estimate_is_ceiled_and_raised_monotonically(self) -> None:
        self.assertEqual(calculate_worker_peak_estimate(4_001), 5_002)
        scheduler = ResourceScheduler(5_002, 20)
        self.assertEqual(
            scheduler.observe_peak(3_000).worker_peak_estimate_bytes,
            5_002,
        )
        self.assertEqual(
            scheduler.observe_peak(6_001).worker_peak_estimate_bytes,
            7_502,
        )

    def test_acceptance_floor_is_strictly_below_fifteen_percent(self) -> None:
        scheduler = ResourceScheduler(1_000, 20)
        self.assertFalse(
            scheduler.acceptance_floor_breached(MemorySnapshot(10_000, 1_500))
        )
        self.assertTrue(
            scheduler.acceptance_floor_breached(MemorySnapshot(10_000, 1_499))
        )

    def test_proc_fallback_forces_one_worker_despite_user_cap(self) -> None:
        self.assertEqual(calculate_max_workers(64, jobs=32, sequential_only=True), 1)
        self.assertEqual(calculate_max_workers(None, jobs=0, sequential_only=True), 1)

    def test_full_plan_reuses_id_126_attempt_one_then_runs_every_baseline(self) -> None:
        attempts = tuple(iter_experiment_attempts(build_matrix(), repetitions=2))
        plan = plan_execution(attempts, complete_matrix=True)

        self.assertEqual(
            (
                plan.calibration.configuration.experiment_id,
                plan.calibration.repetition_attempt,
            ),
            (126, 1),
        )
        self.assertEqual(
            [
                (
                    attempt.configuration.experiment_id,
                    attempt.repetition_attempt,
                )
                for attempt in plan.baseline_wave
            ],
            [
                (experiment_id, repetition_attempt)
                for experiment_id in range(1, 19)
                for repetition_attempt in (1, 2)
            ],
        )
        all_scheduled = (
            (plan.calibration,)
            + plan.baseline_wave
            + plan.remaining_attempts
        )
        self.assertEqual(len(all_scheduled), len(attempts))
        self.assertEqual(len(set(all_scheduled)), len(attempts))
        self.assertEqual(plan.canonical_attempts, attempts)


if __name__ == "__main__":
    unittest.main()
