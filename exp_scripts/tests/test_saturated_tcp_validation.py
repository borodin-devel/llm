"""Strict saturated benchmark JSON validation tests and fixtures."""

from __future__ import annotations

from copy import deepcopy
import json
import math
from pathlib import Path
from tempfile import TemporaryDirectory
import unittest

from saturated_tcp_benchmark.csv_output import StationCsvMetrics
from saturated_tcp_benchmark.matrix import ExperimentConfiguration
from saturated_tcp_benchmark.validation import (
    OutputValidationError,
    load_output_document,
    validate_output_document,
)


VALIDATION_KEYS = (
    "entity_inventory_references_valid",
    "app_agent_totals_consistent",
    "app_peer_totals_consistent",
    "mac_peer_totals_consistent",
    "phy_peer_totals_consistent",
    "ap_station_sender_totals_consistent",
    "overall_matches_windows",
    "unique_phy_payload_within_tagged_payload",
)


def make_configuration(
    *, experiment_id: int = 42, station_count: int = 5
) -> ExperimentConfiguration:
    """Return the matrix coordinate used by strict fixtures."""
    return ExperimentConfiguration(
        experiment_id=experiment_id,
        sta_count_per_bss=station_count,
        rssi_range="medium",
        interference_mode="ap_only_cochannel",
        traffic_mode="ul_dl",
        mimo_mode="su",
    )


def make_effective_configuration(
    configuration: ExperimentConfiguration,
    *,
    repetition_attempt: int = 1,
    repetitions: int = 1,
    run_folder: str = "/tmp/saturated/experiment_042/attempt_1",
) -> dict[str, object]:
    """Return complete effective C++ configuration metadata."""
    return {
        "general": {"output_name": "output.json", "run_folder": run_folder},
        "script": {"repetitions": repetitions},
        "simulation": {"rng_seed": 12345, "rng_run": repetition_attempt},
        "benchmark": {
            "sta_count_per_bss": configuration.sta_count_per_bss,
            "rssi_range": configuration.rssi_range,
            "interference_mode": configuration.interference_mode,
            "traffic_mode": configuration.traffic_mode,
            "mimo_mode": configuration.mimo_mode,
        },
        "wifi": {
            "band": "5GHz",
            "channel_number": 42,
            "bandwidth_mhz": 80,
            "primary_20_index": 0,
            "tx_power_dbm": 20.0,
            "rate_manager": "ns3::MinstrelHtWifiManager",
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
        "logging": {"scenario_level": "info"},
    }


def _sample_distribution() -> dict[str, object]:
    return {
        "sample_count": 0,
        "average_us": None,
        "standard_deviation_us": None,
        "minimum_us": None,
        "maximum_us": None,
    }


def _general_direction() -> dict[str, object]:
    return {
        "estimated_transmitted_tcp_payload_bytes": 0,
        "estimated_matched_tcp_payload_bytes": 0,
        "matched_packet_count": 0,
        "total_transmission_duration_us": 0,
        "average_transmission_duration_us": None,
        "transmission_duration_standard_deviation_us": None,
        "minimum_transmission_duration_us": None,
        "maximum_transmission_duration_us": None,
        "effective_throughput_mbps": None,
        "application_to_phy_delay": _sample_distribution(),
    }


def _app_direction() -> dict[str, object]:
    return {
        "accepted_send_count": 0,
        "accepted_payload_bytes": 0,
        "accepted_throughput_mbps": None,
        "receive_event_count": 0,
        "received_payload_bytes": 0,
        "received_throughput_mbps": None,
        "drop_event_count": 0,
        "dropped_payload_bytes": 0,
        "receive_interarrival_time": _sample_distribution(),
        "agents": [],
        "peers": [],
    }


def _mac_direction() -> dict[str, object]:
    return {
        "estimated_transmit_event_count": 0,
        "estimated_transmitted_tcp_payload_bytes": 0,
        "estimated_transmit_throughput_mbps": None,
        "estimated_receive_event_count": 0,
        "estimated_received_tcp_payload_bytes": 0,
        "estimated_receive_throughput_mbps": None,
        "transmit_drop_count": 0,
        "transmit_drop_packet_bytes": 0,
        "mpdu_drop_count": 0,
        "mpdu_drop_bytes": 0,
        "data_failure_count": 0,
        "final_data_failure_count": 0,
        "mpdu_drops_by_reason": [],
        "peers": [],
    }


def _phy_direction() -> dict[str, object]:
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


def _statistics(theoretical: float, practical: float, contention: float) -> dict[str, object]:
    return {
        "general_stats": {
            "uplink": _general_direction(),
            "downlink": _general_direction(),
        },
        "app_stats": {"uplink": _app_direction(), "downlink": _app_direction()},
        "tcp_stats": {"uplink": {"connections": []}, "downlink": {"connections": []}},
        "mac_stats": {"uplink": _mac_direction(), "downlink": _mac_direction()},
        "phy_stats": {
            "average_theoretical_phy_rate_mbps": theoretical,
            "average_practical_phy_rate_mbps": practical,
            "channel_efficiency": practical / theoretical,
            "contention_fraction": contention,
            "busy_time_us": 0,
            "channel_utilization_percent": None,
            "uplink": _phy_direction(),
            "downlink": _phy_direction(),
        },
    }


def _ap_identity(bss_id: int) -> dict[str, object]:
    return {
        "access_point_id": bss_id,
        "node_id": 100 + bss_id,
        "node_label": f"AP{bss_id}",
        "ipv4": f"10.{bss_id + 1}.0.1",
    }


def _station_identity(bss_id: int, station_index: int) -> dict[str, object]:
    return {
        "access_point_id": bss_id,
        "station_index": station_index,
        "node_id": 200 + bss_id * 30 + station_index,
        "node_label": f"AP{bss_id}/STA{station_index}",
        "ipv4": f"10.{bss_id + 1}.0.{station_index + 2}",
    }


def _entity(identity: dict[str, object], metrics: tuple[float, float, float]) -> dict[str, object]:
    return {**deepcopy(identity), **_statistics(*metrics)}


def _station_metrics(
    bss_id: int, station_index: int, *, overall: bool
) -> tuple[float, float, float]:
    theoretical = 100.0 + bss_id * 10.0 + station_index
    practical = theoretical * 0.5
    window_contention = 0.1 + bss_id * 0.02 + station_index * 0.01
    return theoretical, practical, window_contention * (0.01 if overall else 1.0)


def _bss_metrics(station_metrics: list[tuple[float, float, float]]) -> tuple[float, float, float]:
    theoretical = sum(metric[0] for metric in station_metrics) / len(station_metrics)
    practical = sum(metric[1] for metric in station_metrics) / len(station_metrics)
    contention = sum(metric[2] for metric in station_metrics) / len(station_metrics)
    return theoretical, practical, contention


def make_output_document(
    configuration: ExperimentConfiguration | None = None,
    *,
    repetition_attempt: int = 1,
    repetitions: int = 1,
    run_folder: str = "/tmp/saturated/experiment_042/attempt_1",
) -> tuple[dict[str, object], dict[str, object]]:
    """Build a complete three-BSS shared-schema benchmark fixture."""
    configuration = configuration or make_configuration()
    effective = make_effective_configuration(
        configuration,
        repetition_attempt=repetition_attempt,
        repetitions=repetitions,
        run_folder=run_folder,
    )
    access_point_inventory = [_ap_identity(bss_id) for bss_id in range(3)]
    station_inventory = [
        _station_identity(bss_id, station_index)
        for bss_id in range(3)
        for station_index in range(configuration.sta_count_per_bss)
    ]

    def hierarchy(overall: bool) -> tuple[list[dict[str, object]], list[dict[str, object]]]:
        access_points = []
        stations = []
        for bss_id in range(3):
            bss_station_metrics = []
            for station_index in range(configuration.sta_count_per_bss):
                metrics = _station_metrics(bss_id, station_index, overall=overall)
                bss_station_metrics.append(metrics)
                stations.append(_entity(_station_identity(bss_id, station_index), metrics))
            access_points.append(_entity(_ap_identity(bss_id), _bss_metrics(bss_station_metrics)))
        return access_points, stations

    window_access_points, window_stations = hierarchy(False)
    overall_access_points, overall_stations = hierarchy(True)
    document = {
        "schema_version": 1,
        "measurement_semantics": {
            "access_point_role": "station-derived BSS aggregate",
            "station_role": "per-station transmitted PPDU detail",
            "parent_child_duplication": "intentional",
            "phy_observation_scope": "qualifying station-transmitted PPDUs",
            "phy_rate_source": "actual WifiTxVector and complete PPDU airtime",
            "phy_practical_rate": "qualifying PSDU bits per complete PPDU airtime",
            "contention_fraction": "unioned station EDCA waiting time per interval",
            "sparse_window_absence": "zero station PPDU and contention activity",
            "undefined_derived_values": None,
        },
        "statistics_window_ms": 10,
        "windows": [
            {
                "window_index": 0,
                "window_start_ms": 0.0,
                "window_duration_ms": 10.0,
                "access_points": window_access_points,
                "stations": window_stations,
            }
        ],
        "overall": {
            "access_points": overall_access_points,
            "stations": overall_stations,
        },
        "validation": {key: True for key in VALIDATION_KEYS},
        "experiment_metadata": {
            "configuration": effective,
            "entity_inventory": {
                "access_points": access_point_inventory,
                "stations": station_inventory,
            },
        },
    }
    return document, effective


class SaturatedTcpValidationTest(unittest.TestCase):
    """Protect metadata, shared shape, inventory, formulas, and copying."""

    def setUp(self) -> None:
        self.configuration = make_configuration()
        self.document, self.effective = make_output_document(self.configuration)

    def validate(self, document: dict[str, object] | None = None):
        return validate_output_document(
            document if document is not None else self.document,
            self.configuration,
            repetition_attempt=1,
            expected_configuration=self.effective,
            source_path="fixture.json",
        )

    def test_valid_document_returns_three_exact_fixed_width_rows(self) -> None:
        rows = self.validate()
        self.assertEqual(tuple(row.bss_id for row in rows), (0, 1, 2))
        self.assertEqual(tuple(row.configuration for row in rows), (self.configuration,) * 3)
        self.assertEqual(tuple(row.repetition_attempt for row in rows), (1, 1, 1))
        self.assertEqual(tuple(row.target_rssi_dbm for row in rows), (-50.0, -50.0, -50.0))

        first = rows[0]
        self.assertEqual(first.average_theoretical_phy_rate_mbps, 102.0)
        self.assertEqual(first.average_practical_phy_rate_mbps, 51.0)
        self.assertEqual(first.efficiency, 0.5)
        self.assertAlmostEqual(first.contention_fraction, 0.0012)
        self.assertEqual(len(first.stations), 30)
        self.assertEqual(first.stations[0], StationCsvMetrics(100.0, 50.0, 0.5, 0.001))
        self.assertEqual(
            first.stations[4],
            StationCsvMetrics(104.0, 52.0, 0.5, 0.0014000000000000002),
        )
        self.assertEqual(first.stations[5:], (None,) * 25)

    def test_nullable_overall_station_and_bss_rates_preserve_formulas(self) -> None:
        document = deepcopy(self.document)

        def set_undefined(entity: dict[str, object], contention: float) -> None:
            phy = entity["phy_stats"]
            phy["average_theoretical_phy_rate_mbps"] = None
            phy["average_practical_phy_rate_mbps"] = None
            phy["channel_efficiency"] = None
            phy["contention_fraction"] = contention

        window = document["windows"][0]
        window_station = next(
            station
            for station in window["stations"]
            if station["access_point_id"] == 0 and station["station_index"] == 0
        )
        set_undefined(window_station, 0.1)
        window_ap0 = next(
            access_point
            for access_point in window["access_points"]
            if access_point["access_point_id"] == 0
        )
        window_ap0_phy = window_ap0["phy_stats"]
        window_ap0_phy["average_theoretical_phy_rate_mbps"] = 102.5
        window_ap0_phy["average_practical_phy_rate_mbps"] = 51.25
        window_ap0_phy["channel_efficiency"] = 0.5

        overall_station = next(
            station
            for station in document["overall"]["stations"]
            if station["access_point_id"] == 0 and station["station_index"] == 0
        )
        set_undefined(overall_station, 0.001)
        overall_ap0 = document["overall"]["access_points"][0]
        overall_ap0_phy = overall_ap0["phy_stats"]
        overall_ap0_phy["average_theoretical_phy_rate_mbps"] = 102.5
        overall_ap0_phy["average_practical_phy_rate_mbps"] = 51.25
        overall_ap0_phy["channel_efficiency"] = 0.5

        window["stations"] = [
            station for station in window["stations"] if station["access_point_id"] != 2
        ]
        window["access_points"] = [
            access_point
            for access_point in window["access_points"]
            if access_point["access_point_id"] != 2
        ]
        for station in document["overall"]["stations"]:
            if station["access_point_id"] == 2:
                set_undefined(station, 0.0)
        set_undefined(document["overall"]["access_points"][2], 0.0)

        rows = self.validate(document)
        self.assertEqual(
            rows[0].stations[0],
            StationCsvMetrics(None, None, None, 0.001),
        )
        self.assertEqual(rows[0].average_theoretical_phy_rate_mbps, 102.5)
        self.assertEqual(rows[0].average_practical_phy_rate_mbps, 51.25)
        self.assertEqual(rows[0].efficiency, 0.5)
        self.assertAlmostEqual(rows[0].contention_fraction, 0.0012)
        self.assertIsNone(rows[2].average_theoretical_phy_rate_mbps)
        self.assertIsNone(rows[2].average_practical_phy_rate_mbps)
        self.assertIsNone(rows[2].efficiency)
        self.assertEqual(rows[2].contention_fraction, 0.0)
        self.assertEqual(
            rows[2].stations[:5],
            (StationCsvMetrics(None, None, None, 0.0),) * 5,
        )
        self.assertEqual(rows[2].stations[5:], (None,) * 25)

    def test_repetitions_are_exact_nonbool_uint32_values(self) -> None:
        maximum = (1 << 32) - 1
        valid_document = deepcopy(self.document)
        valid_expected = deepcopy(self.effective)
        valid_document["experiment_metadata"]["configuration"]["script"][
            "repetitions"
        ] = maximum
        valid_expected["script"]["repetitions"] = maximum
        self.assertEqual(
            len(
                validate_output_document(
                    valid_document,
                    self.configuration,
                    repetition_attempt=1,
                    expected_configuration=valid_expected,
                    source_path="fixture.json",
                )
            ),
            3,
        )

        for value in (False, 0, -1, 1 << 32):
            with self.subTest(value=value):
                document = deepcopy(self.document)
                expected = deepcopy(self.effective)
                document["experiment_metadata"]["configuration"]["script"][
                    "repetitions"
                ] = value
                expected["script"]["repetitions"] = value
                with self.assertRaisesRegex(
                    OutputValidationError, "repetitions.*uint32|uint32.*repetitions"
                ):
                    validate_output_document(
                        document,
                        self.configuration,
                        repetition_attempt=1,
                        expected_configuration=expected,
                        source_path="fixture.json",
                    )

    def test_identity_ids_require_integer_uint32_types_before_matching(self) -> None:
        floating_ap = deepcopy(self.document)
        for identity in floating_ap["experiment_metadata"]["entity_inventory"][
            "access_points"
        ]:
            if identity["access_point_id"] == 0:
                identity["access_point_id"] = 0.0
        for identity in floating_ap["experiment_metadata"]["entity_inventory"][
            "stations"
        ]:
            if identity["access_point_id"] == 0:
                identity["access_point_id"] = 0.0
        for hierarchy in (floating_ap["windows"][0], floating_ap["overall"]):
            for entity in hierarchy["access_points"] + hierarchy["stations"]:
                if entity["access_point_id"] == 0:
                    entity["access_point_id"] = 0.0

        floating_station = deepcopy(self.document)
        for identity in floating_station["experiment_metadata"]["entity_inventory"][
            "stations"
        ]:
            if identity["station_index"] == 0:
                identity["station_index"] = 0.0
        for hierarchy in (floating_station["windows"][0], floating_station["overall"]):
            for entity in hierarchy["stations"]:
                if entity["station_index"] == 0:
                    entity["station_index"] = 0.0

        huge_node = deepcopy(self.document)
        huge_node_id = 1 << 32
        huge_node["experiment_metadata"]["entity_inventory"]["access_points"][0][
            "node_id"
        ] = huge_node_id
        huge_node["windows"][0]["access_points"][0]["node_id"] = huge_node_id
        huge_node["overall"]["access_points"][0]["node_id"] = huge_node_id

        floating_output_id = deepcopy(self.document)
        floating_output_id["overall"]["stations"][0]["station_index"] = 0.0

        for name, document in (
            ("floating AP ID", floating_ap),
            ("floating STA ID", floating_station),
            ("oversized node ID", huge_node),
            ("floating output STA ID", floating_output_id),
        ):
            with self.subTest(name=name):
                with self.assertRaisesRegex(OutputValidationError, "uint32"):
                    self.validate(document)

    def test_huge_metric_integer_is_a_validation_error_not_overflow(self) -> None:
        huge = deepcopy(self.document)
        huge["overall"]["stations"][0]["phy_stats"][
            "average_theoretical_phy_rate_mbps"
        ] = 10**10000
        with self.assertRaisesRegex(OutputValidationError, "finite|representable"):
            self.validate(huge)

    def test_sparse_window_rejects_inert_station_and_ap_records(self) -> None:
        for collection in ("stations", "access_points"):
            with self.subTest(collection=collection):
                inert = deepcopy(self.document)
                phy = inert["windows"][0][collection][0]["phy_stats"]
                phy["average_theoretical_phy_rate_mbps"] = None
                phy["average_practical_phy_rate_mbps"] = None
                phy["channel_efficiency"] = None
                phy["contention_fraction"] = 0.0
                with self.assertRaisesRegex(OutputValidationError, "inert|activity"):
                    self.validate(inert)

    def test_exact_root_semantics_metadata_and_flags_are_required(self) -> None:
        mutations = []
        extra_root = deepcopy(self.document)
        extra_root["diagnostics"] = {}
        mutations.append(("root", extra_root))
        reordered_root = {key: self.document[key] for key in reversed(tuple(self.document))}
        mutations.append(("root", reordered_root))
        wrong_schema = deepcopy(self.document)
        wrong_schema["schema_version"] = 2
        mutations.append(("schema_version", wrong_schema))
        wrong_semantics = deepcopy(self.document)
        wrong_semantics["measurement_semantics"]["phy_observation_scope"] = "all PPDUs"
        mutations.append(("measurement_semantics", wrong_semantics))
        wrong_config = deepcopy(self.document)
        wrong_config["experiment_metadata"]["configuration"]["simulation"]["rng_run"] = 2
        mutations.append(("configuration", wrong_config))
        wrong_mode = deepcopy(self.document)
        wrong_mode["experiment_metadata"]["configuration"]["benchmark"]["traffic_mode"] = "ul"
        mutations.append(("configuration", wrong_mode))
        false_flag = deepcopy(self.document)
        false_flag["validation"]["overall_matches_windows"] = False
        mutations.append(("overall_matches_windows", false_flag))
        missing_flag = deepcopy(self.document)
        del missing_flag["validation"]["phy_peer_totals_consistent"]
        mutations.append(("validation", missing_flag))
        for expected_error, document in mutations:
            with self.subTest(expected_error=expected_error):
                with self.assertRaisesRegex(OutputValidationError, expected_error):
                    self.validate(document)

    def test_inventory_and_dense_overall_order_are_exact(self) -> None:
        inventory = self.document["experiment_metadata"]["entity_inventory"]
        mutations = []
        swapped_aps = deepcopy(self.document)
        swapped = swapped_aps["experiment_metadata"]["entity_inventory"]["access_points"]
        swapped[0], swapped[1] = swapped[1], swapped[0]
        mutations.append(swapped_aps)
        duplicate_station = deepcopy(self.document)
        station_inventory = duplicate_station["experiment_metadata"]["entity_inventory"]["stations"]
        station_inventory[1] = deepcopy(station_inventory[0])
        mutations.append(duplicate_station)
        missing_station = deepcopy(self.document)
        missing_station["experiment_metadata"]["entity_inventory"]["stations"].pop()
        mutations.append(missing_station)
        duplicate_bss = deepcopy(self.document)
        duplicate_bss["overall"]["access_points"][1] = deepcopy(
            duplicate_bss["overall"]["access_points"][0]
        )
        mutations.append(duplicate_bss)
        missing_overall_station = deepcopy(self.document)
        missing_overall_station["overall"]["stations"].pop()
        mutations.append(missing_overall_station)
        self.assertEqual(len(inventory["access_points"]), 3)
        self.assertEqual(len(inventory["stations"]), 15)
        for document in mutations:
            with self.subTest(mutation=mutations.index(document)):
                with self.assertRaisesRegex(
                    OutputValidationError, "inventory|order|dense|identity"
                ):
                    self.validate(document)

    def test_station_and_bss_metrics_are_strictly_validated(self) -> None:
        mutations = []
        for field, value in (
            ("average_theoretical_phy_rate_mbps", math.nan),
            ("average_practical_phy_rate_mbps", math.inf),
            ("average_theoretical_phy_rate_mbps", -1.0),
            ("average_practical_phy_rate_mbps", 101.0),
            ("channel_efficiency", 0.9),
            ("contention_fraction", 1.1),
        ):
            changed = deepcopy(self.document)
            changed["overall"]["stations"][0]["phy_stats"][field] = value
            mutations.append((field, changed))
        null_overall = deepcopy(self.document)
        null_overall["overall"]["stations"][0]["phy_stats"][
            "average_theoretical_phy_rate_mbps"
        ] = None
        mutations.append(("presence", null_overall))
        wrong_ap_mean = deepcopy(self.document)
        wrong_ap_mean["overall"]["access_points"][0]["phy_stats"][
            "average_theoretical_phy_rate_mbps"
        ] += 1.0
        wrong_ap_mean["overall"]["access_points"][0]["phy_stats"][
            "channel_efficiency"
        ] = 51.0 / 103.0
        mutations.append(("station-derived", wrong_ap_mean))
        for expected_error, document in mutations:
            with self.subTest(expected_error=expected_error):
                with self.assertRaisesRegex(OutputValidationError, expected_error):
                    self.validate(document)

    def test_shared_entity_shape_and_identity_are_strict(self) -> None:
        extra_field = deepcopy(self.document)
        extra_field["overall"]["stations"][0]["phy_stats"]["tcp_goodput_mbps"] = 1.0
        with self.assertRaisesRegex(OutputValidationError, "phy_stats"):
            self.validate(extra_field)

        wrong_identity = deepcopy(self.document)
        wrong_identity["overall"]["stations"][0]["node_id"] = 999
        with self.assertRaisesRegex(OutputValidationError, "identity"):
            self.validate(wrong_identity)

        nondefault_category = deepcopy(self.document)
        nondefault_category["overall"]["stations"][0]["app_stats"]["uplink"][
            "accepted_send_count"
        ] = 1
        with self.assertRaisesRegex(OutputValidationError, "default"):
            self.validate(nondefault_category)

    def test_loader_rejects_duplicate_keys_and_nonstandard_numbers(self) -> None:
        with TemporaryDirectory() as directory:
            duplicate_path = Path(directory) / "duplicate.json"
            duplicate_path.write_text('{"schema_version":1,"schema_version":1}', encoding="utf-8")
            with self.assertRaisesRegex(OutputValidationError, "duplicate.*schema_version"):
                load_output_document(
                    duplicate_path,
                    self.configuration,
                    repetition_attempt=1,
                    expected_configuration=self.effective,
                )

            nonfinite_path = Path(directory) / "nonfinite.json"
            nonfinite_path.write_text('{"value":NaN}', encoding="utf-8")
            with self.assertRaisesRegex(OutputValidationError, "NaN|non-standard"):
                load_output_document(
                    nonfinite_path,
                    self.configuration,
                    repetition_attempt=1,
                    expected_configuration=self.effective,
                )

    def test_loader_copies_the_same_values_as_direct_validation(self) -> None:
        with TemporaryDirectory() as directory:
            output_path = Path(directory) / "output.json"
            output_path.write_text(json.dumps(self.document), encoding="utf-8")
            loaded_rows = load_output_document(
                output_path,
                self.configuration,
                repetition_attempt=1,
                expected_configuration=self.effective,
            )
        self.assertEqual(loaded_rows, self.validate())

    def test_loader_does_not_follow_output_symlinks(self) -> None:
        with TemporaryDirectory() as directory, TemporaryDirectory() as outside_directory:
            outside_path = Path(outside_directory) / "outside.json"
            outside_path.write_text(json.dumps(self.document), encoding="utf-8")
            output_path = Path(directory) / "output.json"
            output_path.symlink_to(outside_path)
            with self.assertRaisesRegex(OutputValidationError, "symlink|regular"):
                load_output_document(
                    output_path,
                    self.configuration,
                    repetition_attempt=1,
                    expected_configuration=self.effective,
                )


if __name__ == "__main__":
    unittest.main()
