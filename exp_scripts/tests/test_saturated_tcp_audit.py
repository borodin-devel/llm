"""Independent retained-run auditor tests with hand-derived fixture values."""

from __future__ import annotations

import csv
from copy import deepcopy
from io import StringIO
import json
from pathlib import Path
from tempfile import TemporaryDirectory
import unittest

from saturated_tcp_benchmark import audit as audit_module
from saturated_tcp_benchmark.audit import audit_run_directory
from audit_saturated_tcp_results import main


ROOT_KEYS = (
    "schema_version",
    "measurement_semantics",
    "statistics_window_ms",
    "windows",
    "overall",
    "validation",
    "experiment_metadata",
)


def _profile(bytes_value: float, attempts: int, airtime_us: float) -> dict[str, object]:
    return {
        "channel_width_mhz": 40,
        "nss": 1,
        "mcs": 4,
        "transmitted_psdu_bytes": bytes_value,
        "ppdu_attempt_count": attempts,
        "ppdu_airtime_us": airtime_us,
    }


def _default_phy_direction() -> dict[str, object]:
    return {
        "tagged_payload_bytes": 0,
        "unique_tagged_payload_bytes": 0,
        "tagged_mpdu_count": 0,
        "complete_tagged_mpdu_bytes": 0,
        "transmission_attempt_count": 0,
        "retransmission_count": 0,
        "transmission_airtime_us": 0.0,
        "average_data_rate_mbps": None,
        "throughput_mbps": None,
        "peers": [],
    }


def _station_phy(
    bytes_value: float,
    attempts: int,
    airtime_us: float,
    interval_us: float,
) -> dict[str, object]:
    return {
        "dominant_data_phy_rate_mbps": 87.75,
        "dominant_data_profile_share": 1.0,
        "effective_phy_rate_mbps": 20.0,
        "data_tx_rate_over_interval_mbps": bytes_value * 8.0 / interval_us,
        "data_tx_opportunity_gap_fraction": 1.0 - airtime_us / interval_us,
        "data_tx_profile": [_profile(bytes_value, attempts, airtime_us)],
        "mean_dominant_data_phy_rate_mbps": None,
        "mean_effective_phy_rate_mbps": None,
        "aggregate_data_tx_rate_over_interval_mbps": None,
        "busy_time_us": 0,
        "channel_utilization_percent": None,
        "uplink": _default_phy_direction(),
        "downlink": _default_phy_direction(),
    }


def _bss_phy(station_count: int, station_rate: float) -> dict[str, object]:
    return {
        "dominant_data_phy_rate_mbps": None,
        "dominant_data_profile_share": None,
        "effective_phy_rate_mbps": None,
        "data_tx_rate_over_interval_mbps": None,
        "data_tx_opportunity_gap_fraction": None,
        "data_tx_profile": [],
        "mean_dominant_data_phy_rate_mbps": 87.75,
        "mean_effective_phy_rate_mbps": 20.0,
        "aggregate_data_tx_rate_over_interval_mbps": station_count * station_rate,
        "busy_time_us": 0,
        "channel_utilization_percent": None,
        "uplink": _default_phy_direction(),
        "downlink": _default_phy_direction(),
    }


def _station_entity(
    bss_id: int,
    station_index: int,
    bytes_value: float,
    attempts: int,
    airtime_us: float,
    interval_us: float,
) -> dict[str, object]:
    return {
        "access_point_id": bss_id,
        "station_index": station_index,
        "node_id": 1000 + bss_id * 30 + station_index,
        "node_label": f"AP{bss_id}/STA{station_index}",
        "ipv4": f"10.{bss_id + 1}.0.{station_index + 2}",
        "phy_stats": _station_phy(bytes_value, attempts, airtime_us, interval_us),
    }


def _ap_entity(bss_id: int, station_count: int, station_rate: float) -> dict[str, object]:
    return {
        "access_point_id": bss_id,
        "node_id": 100 + bss_id,
        "node_label": f"AP{bss_id}",
        "ipv4": f"10.{bss_id + 1}.0.1",
        "phy_stats": _bss_phy(station_count, station_rate),
    }


def _output_document(experiment_id: int, station_count: int) -> dict[str, object]:
    bytes_value = 1000.0 if station_count == 1 else 300.0
    attempts = 2 if station_count == 1 else 1
    airtime_us = 400.0 if station_count == 1 else 120.0
    window_rate = bytes_value * 8.0 / 10_000.0
    overall_rate = bytes_value * 8.0 / 1_000_000.0
    window_stations = [
        _station_entity(
            bss_id,
            station_index,
            bytes_value,
            attempts,
            airtime_us,
            10_000.0,
        )
        for bss_id in range(3)
        for station_index in range(station_count)
    ]
    overall_stations = [
        _station_entity(
            bss_id,
            station_index,
            bytes_value,
            attempts,
            airtime_us,
            1_000_000.0,
        )
        for bss_id in range(3)
        for station_index in range(station_count)
    ]
    document = {
        "schema_version": 2,
        "measurement_semantics": {
            "access_point_role": "station-derived BSS aggregate",
            "station_role": "per-station transmitted data PPDU detail",
            "parent_child_duplication": "intentional",
            "phy_observation_scope": "qualifying station-transmitted unicast data PPDUs",
            "phy_rate_source": (
                "actual WifiTxVector channel width, NSS, and MCS with fixed "
                "HE SU/GI 3200 ns invariants"
            ),
            "effective_phy_rate": "transmitted data PSDU bits per data PPDU airtime",
            "data_tx_rate_over_interval": "transmitted data PSDU bits per statistics interval",
            "data_tx_opportunity_gap": "time outside station data PPDU airtime",
            "sparse_window_absence": "zero station data profile activity",
            "undefined_derived_values": None,
        },
        "statistics_window_ms": 10,
        "windows": [
            {
                "window_index": 0,
                "window_start_ms": 0.0,
                "window_duration_ms": 10.0,
                "access_points": [
                    _ap_entity(bss_id, station_count, window_rate)
                    for bss_id in range(3)
                ],
                "stations": window_stations,
            }
        ],
        "overall": {
            "access_points": [
                _ap_entity(bss_id, station_count, overall_rate)
                for bss_id in range(3)
            ],
            "stations": overall_stations,
        },
        "validation": {
            "entity_inventory_references_valid": True,
            "app_agent_totals_consistent": True,
            "app_peer_totals_consistent": True,
            "mac_peer_totals_consistent": True,
            "phy_peer_totals_consistent": True,
            "ap_station_sender_totals_consistent": True,
            "overall_matches_windows": True,
            "unique_phy_payload_within_tagged_payload": True,
        },
        "experiment_metadata": {
            "configuration": {
                "general": {"output_name": "output.json", "run_folder": "fixture"},
                "script": {"repetitions": 1},
                "simulation": {"rng_seed": 12345, "rng_run": 1},
                "benchmark": {
                    "sta_count_per_bss": station_count,
                    "rssi_range": "high",
                    "interference_mode": "isolated",
                    "traffic_mode": "ul",
                    "mimo_mode": "su",
                },
                "wifi": {
                    "band": "5GHz",
                    "channel_number": 42,
                    "bandwidth_mhz": 80,
                    "primary_20_index": 0,
                    "tx_power_dbm": 20.0,
                    "rate_manager": "ns3::MinstrelHtWifiManager",
                    "guard_interval_ns": 3200,
                    "rts_cts_threshold_bytes": 0,
                    "antennas": 2,
                    "max_tx_spatial_streams": 2,
                    "max_rx_spatial_streams": 2,
                },
                "tcp": {
                    "congestion_control": "ns3::TcpHighSpeed",
                    "segment_size_bytes": 1460,
                    "send_buffer_bytes": 33554432,
                    "receive_buffer_bytes": 33554432,
                    "wired_rate": "10Gbps",
                    "wired_delay": "0.1ms",
                },
                "statistics": {"window_ms": 10},
            },
            "entity_inventory": {
                "access_points": [
                    {key: value for key, value in _ap_entity(bss_id, 1, 0.0).items() if key != "phy_stats"}
                    for bss_id in range(3)
                ],
                "stations": [
                    {
                        key: value
                        for key, value in _station_entity(
                            bss_id, station_index, 1.0, 1, 1.0, 10_000.0
                        ).items()
                        if key != "phy_stats"
                    }
                    for bss_id in range(3)
                    for station_index in range(station_count)
                ],
            },
        },
    }
    self_order = tuple(document)
    if self_order != ROOT_KEYS:
        raise AssertionError(self_order)
    if experiment_id not in (1, 19):
        raise AssertionError(experiment_id)
    return document


def _csv_header() -> list[str]:
    result = [
        "experiment_id",
        "repetition_attempt",
        "sta_count_per_bss",
        "rssi_range",
        "target_rssi_dbm",
        "interference_mode",
        "traffic_mode",
        "mimo_mode",
        "bss_id",
        "bss_mean_dominant_data_phy_rate_mbps",
        "bss_mean_effective_phy_rate_mbps",
        "bss_aggregate_data_tx_rate_over_interval_mbps",
        "bss_competition_overhead_vs_single_sta",
    ]
    for station_index in range(30):
        result.extend(
            [
                f"sta_{station_index}_dominant_data_phy_rate_mbps",
                f"sta_{station_index}_dominant_data_profile_share",
                f"sta_{station_index}_effective_phy_rate_mbps",
                f"sta_{station_index}_data_tx_rate_over_interval_mbps",
                f"sta_{station_index}_data_tx_opportunity_gap_fraction",
                f"sta_{station_index}_tx_profile",
            ]
        )
    return result


def _csv_row(experiment_id: int, station_count: int, bss_id: int) -> list[object]:
    bytes_value = 1000.0 if station_count == 1 else 300.0
    attempts = 2 if station_count == 1 else 1
    airtime_us = 400.0 if station_count == 1 else 120.0
    interval_rate = bytes_value * 8.0 / 1_000_000.0
    aggregate = station_count * interval_rate
    baseline = 0.008
    row: list[object] = [
        experiment_id,
        1,
        station_count,
        "high",
        -41.5,
        "isolated",
        "ul",
        "su",
        bss_id,
        87.75,
        20.0,
        aggregate,
        1.0 - aggregate / baseline,
    ]
    profile = (
        f"W40_NSS1_MCS4:bytes={format(bytes_value, '.15g')},"
        f"ppdus={attempts},airtime_us={format(airtime_us, '.15g')}"
    )
    for station_index in range(30):
        if station_index < station_count:
            row.extend(
                [
                    87.75,
                    1.0,
                    20.0,
                    interval_rate,
                    1.0 - airtime_us / 1_000_000.0,
                    profile,
                ]
            )
        else:
            row.extend(["", "", "", "", "", ""])
    if len(row) != 193:
        raise AssertionError(len(row))
    return row


def _resource_record(experiment_id: int, peak: int, minimum_percent: float) -> dict[str, object]:
    return {
        "schema_version": 1,
        "experiment_id": experiment_id,
        "repetition_attempt": 1,
        "sample_interval_ms": 100,
        "peak_rss_bytes": peak,
        "minimum_mem_available_bytes": int(minimum_percent * 10_000),
        "minimum_mem_available_percent": minimum_percent,
        "wall_time_seconds": 2.5 + experiment_id / 100.0,
        "exit_code": 0,
        "monitor_mode": "linux_proc",
    }


def _write_mini_run(run_directory: Path) -> None:
    records = []
    for experiment_id, station_count, peak, minimum_percent in (
        (1, 1, 100_000_000, 72.5),
        (19, 5, 220_000_000, 64.0),
    ):
        attempt = run_directory / f"experiment_{experiment_id:03d}" / "attempt_1"
        attempt.mkdir(parents=True)
        (attempt / "output.json").write_text(
            json.dumps(_output_document(experiment_id, station_count)),
            encoding="utf-8",
        )
        (attempt / "stdout.log").write_text("completed\n", encoding="utf-8")
        (attempt / "stderr.log").write_text("", encoding="utf-8")
        record = _resource_record(experiment_id, peak, minimum_percent)
        (attempt / "resource_usage.json").write_text(
            json.dumps(record), encoding="utf-8"
        )
        records.append(record)

    with (run_directory / "results.csv").open(
        "w", encoding="utf-8-sig", newline=""
    ) as output:
        writer = csv.writer(output, delimiter=";", lineterminator="\r\n")
        writer.writerow(_csv_header())
        for experiment_id, station_count in ((1, 1), (19, 5)):
            for bss_id in range(3):
                writer.writerow(_csv_row(experiment_id, station_count, bss_id))

    summary = {
        "schema_version": 1,
        "complete_matrix": False,
        "requested_experiment_ids": [19],
        "executed_experiment_ids": [1, 19],
        "auto_included_baseline_ids": [1],
        "memory_reserve_percent": 20,
        "calibrated_peak_rss_bytes": 100_000_000,
        "worker_peak_estimate_bytes": 275_000_000,
        "maximum_parallel_workers": 2,
        "minimum_mem_available_bytes": 640_000,
        "minimum_mem_available_percent": 64.0,
        "attempts": records,
    }
    (run_directory / "resource_summary.json").write_text(
        json.dumps(summary), encoding="utf-8"
    )


class SaturatedTcpAuditTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = TemporaryDirectory()
        self.run_directory = Path(self.temporary.name) / "scripted_exp_fixture"
        self.run_directory.mkdir()
        _write_mini_run(self.run_directory)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def audit(self):
        return audit_run_directory(self.run_directory)

    def test_zero_discrepancy_report_reconstructs_all_independent_values(self) -> None:
        report = self.audit()

        self.assertEqual(report.discrepancies, ())
        self.assertEqual(report.experiment_attempt_count, 2)
        self.assertEqual(report.output_json_count, 2)
        self.assertEqual(report.csv_data_row_count, 6)
        self.assertEqual(report.csv_column_count, 193)
        self.assertEqual(report.resource_usage_count, 2)
        self.assertEqual(report.stdout_log_count, 2)
        self.assertEqual(report.stderr_log_count, 2)
        self.assertEqual(report.null_csv_cell_count, 972)
        self.assertAlmostEqual(report.signed_baseline_minimum, -0.5)
        self.assertEqual(report.signed_baseline_maximum, 0.0)
        self.assertEqual(report.minimum_mem_available_percent, 64.0)
        self.assertEqual(report.peak_rss_bytes, 220_000_000)
        self.assertEqual(report.maximum_parallel_workers, 2)

    def test_cli_emits_machine_readable_report_and_nonzero_on_discrepancy(self) -> None:
        output = StringIO()
        error = StringIO()
        self.assertEqual(main([str(self.run_directory)], output=output, error=error), 0)
        report = json.loads(output.getvalue())
        self.assertEqual(report["discrepancy_count"], 0)
        self.assertEqual(report["csv_column_count"], 193)
        self.assertEqual(error.getvalue(), "")

        (self.run_directory / "experiment_019/attempt_1/stderr.log").unlink()
        output = StringIO()
        self.assertEqual(main([str(self.run_directory)], output=output, error=error), 1)
        self.assertGreater(json.loads(output.getvalue())["discrepancy_count"], 0)

    def test_detects_json_formula_profile_and_bss_mutations(self) -> None:
        output_path = self.run_directory / "experiment_019/attempt_1/output.json"
        original = json.loads(output_path.read_text(encoding="utf-8"))
        cases = []

        changed = deepcopy(original)
        changed["overall"]["stations"][0]["phy_stats"][
            "dominant_data_phy_rate_mbps"
        ] = 88.0
        cases.append(("dominant_data_phy_rate_mbps", changed))

        changed = deepcopy(original)
        station_phy = changed["overall"]["stations"][0]["phy_stats"]
        station_phy["data_tx_profile"][0]["transmitted_psdu_bytes"] = 301.0
        station_phy["data_tx_profile"][0]["ppdu_airtime_us"] = 120.4
        station_phy["data_tx_rate_over_interval_mbps"] = 0.002408
        station_phy["data_tx_opportunity_gap_fraction"] = 0.9998796
        changed["overall"]["access_points"][0]["phy_stats"][
            "aggregate_data_tx_rate_over_interval_mbps"
        ] = 0.012008
        cases.append(("reproduce windows", changed))

        changed = deepcopy(original)
        changed["overall"]["access_points"][0]["phy_stats"][
            "aggregate_data_tx_rate_over_interval_mbps"
        ] = 0.1
        cases.append(("BSS", changed))

        for expected, document in cases:
            with self.subTest(expected=expected):
                output_path.write_text(json.dumps(document), encoding="utf-8")
                self.assertTrue(
                    any(expected in discrepancy for discrepancy in self.audit().discrepancies)
                )
        output_path.write_text(json.dumps(original), encoding="utf-8")

    def test_detects_csv_cell_baseline_order_and_transport_mutations(self) -> None:
        csv_path = self.run_directory / "results.csv"
        original = csv_path.read_bytes()
        text = original.decode("utf-8-sig")
        rows = list(csv.reader(text.splitlines(), delimiter=";"))

        cases: list[tuple[str, bytes]] = []
        changed = deepcopy(rows)
        changed[1][9] = "999"
        cases.append(("cell", _encode_rows(changed)))
        changed = deepcopy(rows)
        changed[4][12] = "0.25"
        cases.append(("baseline", _encode_rows(changed)))
        changed = deepcopy(rows)
        changed[1], changed[2] = changed[2], changed[1]
        cases.append(("order", _encode_rows(changed)))
        changed = deepcopy(rows)
        changed[0][0] = "wrong_experiment_id"
        cases.append(("header", _encode_rows(changed)))
        cases.append(("UTF-8 BOM", original[3:]))
        cases.append(("CRLF", original.replace(b"\r\n", b"\n")))
        cases.append(("CRLF", original.replace(b";", b";\r", 1)))

        for expected, content in cases:
            with self.subTest(expected=expected):
                csv_path.write_bytes(content)
                self.assertTrue(
                    any(expected in discrepancy for discrepancy in self.audit().discrepancies)
                )
        csv_path.write_bytes(original)

    def test_detects_resource_manifest_and_log_mutations(self) -> None:
        usage_path = (
            self.run_directory / "experiment_019/attempt_1/resource_usage.json"
        )
        usage_original = usage_path.read_bytes()
        usage_path.unlink()
        discrepancies = self.audit().discrepancies
        self.assertTrue(any("resource" in item for item in discrepancies))
        usage_path.write_bytes(usage_original)

        summary_path = self.run_directory / "resource_summary.json"
        summary = json.loads(summary_path.read_text(encoding="utf-8"))
        summary["auto_included_baseline_ids"] = []
        summary_path.write_text(json.dumps(summary), encoding="utf-8")
        self.assertTrue(any("manifest" in item for item in self.audit().discrepancies))
        _write_mini_run_replacing_summary(self.run_directory)

        usage = json.loads(usage_original)
        usage["exit_code"] = 9
        usage_path.write_text(json.dumps(usage), encoding="utf-8")
        summary = json.loads(summary_path.read_text(encoding="utf-8"))
        summary["attempts"][1]["exit_code"] = 9
        summary_path.write_text(json.dumps(summary), encoding="utf-8")
        self.assertTrue(any("exit_code" in item for item in self.audit().discrepancies))
        usage_path.write_bytes(usage_original)
        _write_mini_run_replacing_summary(self.run_directory)

        log_path = self.run_directory / "experiment_019/attempt_1/stderr.log"
        log_path.unlink()
        self.assertTrue(any("log" in item for item in self.audit().discrepancies))

    def test_rejects_duplicate_keys_and_nonstandard_numbers(self) -> None:
        output_path = self.run_directory / "experiment_001/attempt_1/output.json"
        original = output_path.read_text(encoding="utf-8")
        duplicate = original.replace(
            '"schema_version": 2', '"schema_version": 2, "schema_version": 2', 1
        )
        output_path.write_text(duplicate, encoding="utf-8")
        self.assertTrue(any("duplicate" in item for item in self.audit().discrepancies))

        output_path.write_text(original.replace("87.75", "NaN", 1), encoding="utf-8")
        self.assertTrue(
            any("non-standard JSON number" in item for item in self.audit().discrepancies)
        )

    def test_requires_exact_semantics_and_validation_contracts(self) -> None:
        output_path = self.run_directory / "experiment_001/attempt_1/output.json"
        original = json.loads(output_path.read_text(encoding="utf-8"))

        changed = deepcopy(original)
        changed["measurement_semantics"]["extra"] = "accepted by subset checks"
        output_path.write_text(json.dumps(changed), encoding="utf-8")
        self.assertTrue(any("semantics" in item for item in self.audit().discrepancies))

        changed = deepcopy(original)
        changed["validation"] = {"arbitrary_all_true": True}
        output_path.write_text(json.dumps(changed), encoding="utf-8")
        self.assertTrue(any("validation" in item for item in self.audit().discrepancies))

    def test_rejects_a_completely_empty_sparse_window(self) -> None:
        output_path = self.run_directory / "experiment_001/attempt_1/output.json"
        document = json.loads(output_path.read_text(encoding="utf-8"))
        document["windows"][0]["access_points"] = []
        document["windows"][0]["stations"] = []
        output_path.write_text(json.dumps(document), encoding="utf-8")

        self.assertTrue(
            any("empty sparse window" in item for item in self.audit().discrepancies)
        )

    def test_output_schema_version_requires_exact_integer_two(self) -> None:
        output_path = self.run_directory / "experiment_001/attempt_1/output.json"
        original = json.loads(output_path.read_text(encoding="utf-8"))
        for invalid in (2.0, True):
            with self.subTest(invalid=invalid):
                changed = deepcopy(original)
                changed["schema_version"] = invalid
                output_path.write_text(json.dumps(changed), encoding="utf-8")
                self.assertTrue(
                    any(
                        "schema_version must be integer 2" in item
                        for item in self.audit().discrepancies
                    )
                )

    def test_resource_schema_versions_require_exact_integer_one(self) -> None:
        usage_path = self.run_directory / "experiment_001/attempt_1/resource_usage.json"
        summary_path = self.run_directory / "resource_summary.json"
        original_usage = json.loads(usage_path.read_text(encoding="utf-8"))
        original_summary = json.loads(summary_path.read_text(encoding="utf-8"))
        for invalid in (1.0, True):
            with self.subTest(document="usage", invalid=invalid):
                usage = deepcopy(original_usage)
                summary = deepcopy(original_summary)
                usage["schema_version"] = invalid
                summary["attempts"][0]["schema_version"] = invalid
                usage_path.write_text(json.dumps(usage), encoding="utf-8")
                summary_path.write_text(json.dumps(summary), encoding="utf-8")
                self.assertTrue(
                    any(
                        "resource usage schema_version must be integer 1" in item
                        for item in self.audit().discrepancies
                    )
                )
            with self.subTest(document="summary", invalid=invalid):
                usage_path.write_text(json.dumps(original_usage), encoding="utf-8")
                summary = deepcopy(original_summary)
                summary["schema_version"] = invalid
                summary_path.write_text(json.dumps(summary), encoding="utf-8")
                self.assertTrue(
                    any(
                        "resource manifest schema_version must be integer 1" in item
                        for item in self.audit().discrepancies
                    )
                )

    def test_phy_stats_rejects_every_trailing_key(self) -> None:
        output_path = self.run_directory / "experiment_001/attempt_1/output.json"
        original = json.loads(output_path.read_text(encoding="utf-8"))
        paths = (
            original["overall"]["stations"][0]["phy_stats"],
            original["overall"]["access_points"][0]["phy_stats"],
        )
        for role, original_phy in zip(("station", "BSS"), paths):
            with self.subTest(role=role):
                changed = deepcopy(original)
                entities = (
                    changed["overall"]["stations"]
                    if role == "station"
                    else changed["overall"]["access_points"]
                )
                entities[0]["phy_stats"]["unexpected_trailing_key"] = None
                output_path.write_text(json.dumps(changed), encoding="utf-8")
                self.assertTrue(
                    any(
                        f"{role} PHY field order is invalid" in item
                        for item in self.audit().discrepancies
                    )
                )

    def test_rejects_each_fixed_wifi_and_tcp_metadata_mutation(self) -> None:
        output_path = self.run_directory / "experiment_001/attempt_1/output.json"
        original = json.loads(output_path.read_text(encoding="utf-8"))
        mutations = (
            ("wifi", "band", "2.4GHz"),
            ("wifi", "channel_number", 42.0),
            ("wifi", "bandwidth_mhz", 40),
            ("wifi", "primary_20_index", 1),
            ("wifi", "tx_power_dbm", 19.0),
            ("wifi", "rate_manager", "ns3::IdealWifiManager"),
            ("wifi", "guard_interval_ns", 1600),
            ("wifi", "rts_cts_threshold_bytes", 1),
            ("wifi", "antennas", 1),
            ("wifi", "max_tx_spatial_streams", 1),
            ("wifi", "max_rx_spatial_streams", 1),
            ("tcp", "congestion_control", "ns3::TcpCubic"),
            ("tcp", "segment_size_bytes", 1448),
            ("tcp", "send_buffer_bytes", 1024),
            ("tcp", "receive_buffer_bytes", 1024),
            ("tcp", "wired_rate", "1Gbps"),
            ("tcp", "wired_delay", "1ms"),
        )
        for section, field, value in mutations:
            with self.subTest(section=section, field=field):
                changed = deepcopy(original)
                changed["experiment_metadata"]["configuration"][section][field] = value
                output_path.write_text(json.dumps(changed), encoding="utf-8")
                self.assertTrue(
                    any(
                        f"{section} fixed metadata" in item.lower()
                        for item in self.audit().discrepancies
                    )
                )

    def test_rejects_measurement_duration_and_window_invariant_mutations(self) -> None:
        output_path = self.run_directory / "experiment_001/attempt_1/output.json"
        original = json.loads(output_path.read_text(encoding="utf-8"))

        changed = deepcopy(original)
        changed["statistics_window_ms"] = 11
        changed["experiment_metadata"]["configuration"]["statistics"]["window_ms"] = 11
        mutations = [("divide one second", changed)]

        changed = deepcopy(original)
        changed["windows"][0]["window_index"] = 100
        changed["windows"][0]["window_start_ms"] = 1000.0
        mutations.append(("order/range", changed))

        changed = deepcopy(original)
        changed["windows"][0]["window_duration_ms"] = 9.0
        mutations.append(("position", changed))

        changed = deepcopy(original)
        changed["experiment_metadata"]["configuration"]["statistics"]["window_ms"] = 20
        mutations.append(("window metadata", changed))

        for expected, document in mutations:
            with self.subTest(expected=expected):
                output_path.write_text(json.dumps(document), encoding="utf-8")
                self.assertTrue(
                    any(expected in item for item in self.audit().discrepancies)
                )

    def test_resource_sample_interval_requires_exact_integer_100(self) -> None:
        usage_path = self.run_directory / "experiment_019/attempt_1/resource_usage.json"
        summary_path = self.run_directory / "resource_summary.json"
        original_usage = json.loads(usage_path.read_text(encoding="utf-8"))
        original_summary = json.loads(summary_path.read_text(encoding="utf-8"))
        for invalid in (99, 100.0):
            with self.subTest(invalid=invalid):
                usage = deepcopy(original_usage)
                summary = deepcopy(original_summary)
                usage["sample_interval_ms"] = invalid
                summary["attempts"][1]["sample_interval_ms"] = invalid
                usage_path.write_text(json.dumps(usage), encoding="utf-8")
                summary_path.write_text(json.dumps(summary), encoding="utf-8")
                self.assertTrue(
                    any(
                        "sample_interval_ms must be integer 100" in item
                        for item in self.audit().discrepancies
                    )
                )

    def test_linux_proc_requires_numeric_rss_and_memory_minima(self) -> None:
        usage_path = self.run_directory / "experiment_019/attempt_1/resource_usage.json"
        summary_path = self.run_directory / "resource_summary.json"
        original_usage = json.loads(usage_path.read_text(encoding="utf-8"))
        original_summary = json.loads(summary_path.read_text(encoding="utf-8"))
        for field in (
            "peak_rss_bytes",
            "minimum_mem_available_bytes",
            "minimum_mem_available_percent",
        ):
            with self.subTest(field=field):
                usage = deepcopy(original_usage)
                summary = deepcopy(original_summary)
                usage[field] = None
                summary["attempts"][1][field] = None
                usage_path.write_text(json.dumps(usage), encoding="utf-8")
                summary_path.write_text(json.dumps(summary), encoding="utf-8")
                self.assertTrue(
                    any(
                        f"linux_proc requires numeric {field}" in item
                        for item in self.audit().discrepancies
                    )
                )

    def test_sequential_fallback_requires_null_resources_and_one_worker(self) -> None:
        usage_paths = (
            self.run_directory / "experiment_001/attempt_1/resource_usage.json",
            self.run_directory / "experiment_019/attempt_1/resource_usage.json",
        )
        summary_path = self.run_directory / "resource_summary.json"
        usages = [json.loads(path.read_text(encoding="utf-8")) for path in usage_paths]
        summary = json.loads(summary_path.read_text(encoding="utf-8"))
        nullable_fields = (
            "peak_rss_bytes",
            "minimum_mem_available_bytes",
            "minimum_mem_available_percent",
        )
        for usage in usages:
            usage["monitor_mode"] = "sequential_fallback"
            for field in nullable_fields:
                usage[field] = None
        summary["attempts"] = deepcopy(usages)
        for field in (
            "calibrated_peak_rss_bytes",
            "worker_peak_estimate_bytes",
            "minimum_mem_available_bytes",
            "minimum_mem_available_percent",
        ):
            summary[field] = None
        summary["maximum_parallel_workers"] = 1

        for path, usage in zip(usage_paths, usages):
            path.write_text(json.dumps(usage), encoding="utf-8")
        summary_path.write_text(json.dumps(summary), encoding="utf-8")
        self.assertEqual(self.audit().discrepancies, ())

        mutations = [
            ("peak_rss_bytes", 1),
            ("minimum_mem_available_bytes", 1),
            ("minimum_mem_available_percent", 1.0),
        ]
        for field, value in mutations:
            with self.subTest(field=field):
                changed_usage = deepcopy(usages[1])
                changed_usage[field] = value
                usage_paths[1].write_text(json.dumps(changed_usage), encoding="utf-8")
                changed_summary = deepcopy(summary)
                changed_summary["attempts"][1][field] = value
                summary_path.write_text(json.dumps(changed_summary), encoding="utf-8")
                self.assertTrue(
                    any(
                        f"sequential_fallback requires null {field}" in item
                        for item in self.audit().discrepancies
                    )
                )
                usage_paths[1].write_text(json.dumps(usages[1]), encoding="utf-8")

        for field in (
            "calibrated_peak_rss_bytes",
            "worker_peak_estimate_bytes",
            "minimum_mem_available_bytes",
            "minimum_mem_available_percent",
        ):
            with self.subTest(summary_field=field):
                changed_summary = deepcopy(summary)
                changed_summary[field] = 1.0
                summary_path.write_text(json.dumps(changed_summary), encoding="utf-8")
                self.assertTrue(
                    any(
                        f"sequential_fallback requires null {field}" in item
                        for item in self.audit().discrepancies
                    )
                )

        summary_path.write_text(json.dumps(summary), encoding="utf-8")
        changed_summary = deepcopy(summary)
        changed_summary["maximum_parallel_workers"] = 2
        summary_path.write_text(json.dumps(changed_summary), encoding="utf-8")
        self.assertTrue(
            any(
                "sequential_fallback requires exactly one worker" in item
                for item in self.audit().discrepancies
            )
        )

    def test_number_conversion_failures_become_path_bearing_discrepancies(self) -> None:
        class TypeFailingInt(int):
            def __float__(self):
                raise TypeError("injected conversion failure")

        class ValueFailingInt(int):
            def __float__(self):
                raise ValueError("injected conversion failure")

        for value in (10**400, TypeFailingInt(1), ValueFailingInt(1)):
            with self.subTest(value_type=type(value).__name__):
                with self.assertRaisesRegex(ValueError, "fixture.json: metric"):
                    audit_module._number(value, Path("fixture.json"), "metric")

    def test_cli_returns_nonzero_instead_of_crashing_on_huge_json_integer(self) -> None:
        output_path = self.run_directory / "experiment_001/attempt_1/output.json"
        document = json.loads(output_path.read_text(encoding="utf-8"))
        document["windows"][0]["window_start_ms"] = 10**400
        output_path.write_text(json.dumps(document), encoding="utf-8")
        output = StringIO()
        error = StringIO()

        self.assertEqual(main([str(self.run_directory)], output=output, error=error), 1)
        report = json.loads(output.getvalue())
        self.assertGreater(report["discrepancy_count"], 0)
        self.assertTrue(
            any("window 0 start" in item for item in report["discrepancies"])
        )
        self.assertEqual(error.getvalue(), "")

    def test_manifest_requires_complete_unique_attempt_lattice(self) -> None:
        dependent = self.run_directory / "experiment_019"
        for path in sorted(dependent.rglob("*"), reverse=True):
            if path.is_file():
                path.unlink()
            else:
                path.rmdir()
        dependent.rmdir()

        summary_path = self.run_directory / "resource_summary.json"
        summary = json.loads(summary_path.read_text(encoding="utf-8"))
        summary["attempts"] = summary["attempts"][:1]
        summary_path.write_text(json.dumps(summary), encoding="utf-8")
        csv_path = self.run_directory / "results.csv"
        rows = list(csv.reader(csv_path.read_text(encoding="utf-8-sig").splitlines(), delimiter=";"))
        csv_path.write_bytes(_encode_rows(rows[:4]))

        self.assertTrue(any("lattice" in item for item in self.audit().discrepancies))

    def test_output_declared_repetitions_define_the_exact_attempt_lattice(self) -> None:
        output_paths = (
            self.run_directory / "experiment_001/attempt_1/output.json",
            self.run_directory / "experiment_019/attempt_1/output.json",
        )
        originals = [json.loads(path.read_text(encoding="utf-8")) for path in output_paths]

        for path, document in zip(output_paths, deepcopy(originals)):
            document["experiment_metadata"]["configuration"]["script"][
                "repetitions"
            ] = 2
            path.write_text(json.dumps(document), encoding="utf-8")
        self.assertTrue(
            any("declared repetition lattice" in item for item in self.audit().discrepancies)
        )

        for path, document in zip(output_paths, deepcopy(originals)):
            path.write_text(json.dumps(document), encoding="utf-8")
        inconsistent = deepcopy(originals[1])
        inconsistent["experiment_metadata"]["configuration"]["script"][
            "repetitions"
        ] = 2
        output_paths[1].write_text(json.dumps(inconsistent), encoding="utf-8")
        self.assertTrue(
            any("consistent script.repetitions" in item for item in self.audit().discrepancies)
        )

        for path, document in zip(output_paths, deepcopy(originals)):
            document["experiment_metadata"]["configuration"]["script"][
                "repetitions"
            ] = 0
            path.write_text(json.dumps(document), encoding="utf-8")
        self.assertTrue(
            any("script.repetitions" in item for item in self.audit().discrepancies)
        )

    def test_manifest_rejects_empty_and_duplicate_selection_or_attempt_keys(self) -> None:
        summary_path = self.run_directory / "resource_summary.json"
        original = json.loads(summary_path.read_text(encoding="utf-8"))
        mutations = []
        changed = deepcopy(original)
        changed["requested_experiment_ids"] = []
        mutations.append(("requested_experiment_ids must be nonempty", changed))
        changed = deepcopy(original)
        changed["executed_experiment_ids"] = [1, 1, 19]
        mutations.append(("executed_experiment_ids must be unique", changed))
        changed = deepcopy(original)
        changed["attempts"].append(deepcopy(changed["attempts"][0]))
        mutations.append(("attempt keys must be unique", changed))

        for expected, summary in mutations:
            with self.subTest(expected=expected):
                summary_path.write_text(json.dumps(summary), encoding="utf-8")
                self.assertTrue(
                    any(expected in item for item in self.audit().discrepancies)
                )

    def test_resource_summary_aggregates_are_independently_reconstructed(self) -> None:
        summary_path = self.run_directory / "resource_summary.json"
        original = json.loads(summary_path.read_text(encoding="utf-8"))
        mutations = (
            ("memory_reserve_percent", 14),
            ("calibrated_peak_rss_bytes", 999),
            ("worker_peak_estimate_bytes", -1),
            ("worker_peak_estimate_bytes", 10**12),
            ("minimum_mem_available_bytes", 999999),
            ("minimum_mem_available_percent", 999.0),
        )
        for field, value in mutations:
            with self.subTest(field=field):
                changed = deepcopy(original)
                changed[field] = value
                summary_path.write_text(json.dumps(changed), encoding="utf-8")
                self.assertTrue(any(field in item for item in self.audit().discrepancies))

    def test_prelaunch_failure_summary_accepts_zero_parallel_workers(self) -> None:
        for experiment_directory in sorted(
            self.run_directory.glob("experiment_*"), reverse=True
        ):
            for path in sorted(experiment_directory.rglob("*"), reverse=True):
                if path.is_file():
                    path.unlink()
                else:
                    path.rmdir()
            experiment_directory.rmdir()
        (self.run_directory / "results.csv").write_bytes(_encode_rows([_csv_header()]))
        summary_path = self.run_directory / "resource_summary.json"
        summary = json.loads(summary_path.read_text(encoding="utf-8"))
        summary["attempts"] = []
        summary["calibrated_peak_rss_bytes"] = None
        summary["worker_peak_estimate_bytes"] = None
        summary["maximum_parallel_workers"] = 0
        summary["minimum_mem_available_bytes"] = None
        summary["minimum_mem_available_percent"] = None
        summary_path.write_text(json.dumps(summary), encoding="utf-8")

        report = self.audit()

        self.assertEqual(report.maximum_parallel_workers, 0)
        self.assertFalse(
            any(
                "maximum_parallel_workers is invalid" in item
                for item in report.discrepancies
            )
        )


def _encode_rows(rows: list[list[str]]) -> bytes:
    from io import StringIO

    output = StringIO(newline="")
    writer = csv.writer(output, delimiter=";", lineterminator="\r\n")
    writer.writerows(rows)
    return b"\xef\xbb\xbf" + output.getvalue().encode("utf-8")


def _write_mini_run_replacing_summary(run_directory: Path) -> None:
    records = [
        _resource_record(1, 100_000_000, 72.5),
        _resource_record(19, 220_000_000, 64.0),
    ]
    summary = {
        "schema_version": 1,
        "complete_matrix": False,
        "requested_experiment_ids": [19],
        "executed_experiment_ids": [1, 19],
        "auto_included_baseline_ids": [1],
        "memory_reserve_percent": 20,
        "calibrated_peak_rss_bytes": 100_000_000,
        "worker_peak_estimate_bytes": 275_000_000,
        "maximum_parallel_workers": 2,
        "minimum_mem_available_bytes": 640_000,
        "minimum_mem_available_percent": 64.0,
        "attempts": records,
    }
    (run_directory / "resource_summary.json").write_text(
        json.dumps(summary), encoding="utf-8"
    )


if __name__ == "__main__":
    unittest.main()
