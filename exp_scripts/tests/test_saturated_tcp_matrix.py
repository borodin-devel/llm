"""Exact saturated TCP matrix contract tests."""

from __future__ import annotations

import unittest

from saturated_tcp_benchmark.matrix import (
    INTERFERENCE_MODES,
    MIMO_MODES,
    RSSI_RANGES,
    STA_COUNTS,
    TRAFFIC_MODES,
    ExperimentAttempt,
    ExperimentConfiguration,
    build_matrix,
    iter_experiment_attempts,
    target_rssi_dbm,
)


class SaturatedTcpMatrixTest(unittest.TestCase):
    """Protect the fixed SU-only product and attempt mapping."""

    def test_matrix_has_exact_constants_order_and_stable_ids(self) -> None:
        self.assertEqual(STA_COUNTS, (5, 10, 15, 20, 25, 30))
        self.assertEqual(RSSI_RANGES, ("high", "medium", "low"))
        self.assertEqual(INTERFERENCE_MODES, ("isolated", "ap_only_cochannel"))
        self.assertEqual(TRAFFIC_MODES, ("ul", "dl", "ul_dl"))
        self.assertEqual(MIMO_MODES, ("su",))

        expected_coordinates = []
        for station_count in (5, 10, 15, 20, 25, 30):
            for rssi_range in ("high", "medium", "low"):
                for interference_mode in ("isolated", "ap_only_cochannel"):
                    for traffic_mode in ("ul", "dl", "ul_dl"):
                        expected_coordinates.append(
                            (
                                station_count,
                                rssi_range,
                                interference_mode,
                                traffic_mode,
                                "su",
                            )
                        )

        configurations = build_matrix()
        self.assertIsInstance(configurations, tuple)
        self.assertEqual(len(configurations), 108)
        self.assertEqual(
            [configuration.experiment_id for configuration in configurations],
            list(range(1, 109)),
        )
        self.assertEqual(
            [
                (
                    configuration.sta_count_per_bss,
                    configuration.rssi_range,
                    configuration.interference_mode,
                    configuration.traffic_mode,
                    configuration.mimo_mode,
                )
                for configuration in configurations
            ],
            expected_coordinates,
        )

    def test_attempts_preserve_experiment_id_and_map_rng_run(self) -> None:
        configurations = build_matrix()[:2]
        self.assertEqual(
            tuple(iter_experiment_attempts(configurations[:1])),
            (ExperimentAttempt(configurations[0], 1, 1),),
        )
        self.assertEqual(
            tuple(iter_experiment_attempts(configurations, repetitions=2)),
            (
                ExperimentAttempt(configurations[0], 1, 1),
                ExperimentAttempt(configurations[0], 2, 2),
                ExperimentAttempt(configurations[1], 1, 1),
                ExperimentAttempt(configurations[1], 2, 2),
            ),
        )
        self.assertEqual(
            [
                attempt.configuration.experiment_id
                for attempt in iter_experiment_attempts(configurations, repetitions=2)
            ],
            [1, 1, 2, 2],
        )

    def test_mu_and_invalid_attempt_counts_are_rejected(self) -> None:
        with self.assertRaisesRegex(ValueError, "unsupported.*mu"):
            build_matrix(mimo_modes=("mu",))
        with self.assertRaisesRegex(ValueError, "unsupported.*mu"):
            build_matrix(mimo_modes=("su", "mu"))
        for repetitions in (0, -1, True):
            with self.subTest(repetitions=repetitions):
                with self.assertRaisesRegex(ValueError, "repetitions"):
                    tuple(iter_experiment_attempts(build_matrix()[:1], repetitions))

    def test_target_rssi_mapping_is_exact(self) -> None:
        self.assertEqual(target_rssi_dbm("high"), -41.5)
        self.assertEqual(target_rssi_dbm("medium"), -50.0)
        self.assertEqual(target_rssi_dbm("low"), -60.0)
        with self.assertRaisesRegex(ValueError, "RSSI"):
            target_rssi_dbm("HIGH")

    def test_boundaries_are_frozen_dataclasses(self) -> None:
        configuration = ExperimentConfiguration(1, 5, "high", "isolated", "ul", "su")
        attempt = ExperimentAttempt(configuration, 1, 1)
        with self.assertRaises((AttributeError, TypeError)):
            configuration.experiment_id = 2  # type: ignore[misc]
        with self.assertRaises((AttributeError, TypeError)):
            attempt.rng_run = 2  # type: ignore[misc]


if __name__ == "__main__":
    unittest.main()
