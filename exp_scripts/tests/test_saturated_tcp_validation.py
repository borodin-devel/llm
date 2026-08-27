"""Strict schema-v2 saturated benchmark JSON validation tests and fixtures."""

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
    *,
    experiment_id: int = 12,
    station_count: int = 1,
    traffic_mode: str = "ul_dl",
) -> ExperimentConfiguration:
    """Return the literal matrix coordinate used by strict fixtures."""
    return ExperimentConfiguration(
        experiment_id=experiment_id,
        sta_count_per_bss=station_count,
        rssi_range="medium",
        interference_mode="ap_only_cochannel",
        traffic_mode=traffic_mode,
        mimo_mode="su",
    )


def make_effective_configuration(
    configuration: ExperimentConfiguration,
    *,
    repetition_attempt: int = 1,
    repetitions: int = 1,
    run_folder: str = "/tmp/saturated/experiment_012/attempt_1",
) -> dict[str, object]:
    """Return complete ordered C++ configuration metadata including v2 invariants."""
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


def _categories(phy: dict[str, object]) -> dict[str, object]:
    return {
        "general_stats": {"uplink": _general_direction(), "downlink": _general_direction()},
        "app_stats": {"uplink": _app_direction(), "downlink": _app_direction()},
        "tcp_stats": {"uplink": {"connections": []}, "downlink": {"connections": []}},
        "mac_stats": {"uplink": _mac_direction(), "downlink": _mac_direction()},
        "phy_stats": {
            **phy,
            "busy_time_us": 0,
            "channel_utilization_percent": None,
            "uplink": _phy_direction(),
            "downlink": _phy_direction(),
        },
    }


def _active_station_phy(bss_id: int, *, overall: bool) -> dict[str, object]:
    """Return hand-derived profile and station values without validator helpers."""
    if bss_id == 0:
        profiles = [
            {
                "channel_width_mhz": 20,
                "nss": 1,
                "mcs": 9,
                "transmitted_psdu_bytes": 100.0,
                "ppdu_attempt_count": 2,
                "ppdu_airtime_us": 40.0,
            },
            {
                "channel_width_mhz": 80,
                "nss": 2,
                "mcs": 11,
                "transmitted_psdu_bytes": 300.0,
                "ppdu_attempt_count": 4,
                "ppdu_airtime_us": 120.0,
            },
        ]
        return {
            "dominant_data_phy_rate_mbps": 1020.833334,
            "dominant_data_profile_share": 0.75,
            "effective_phy_rate_mbps": 20.0,
            "data_tx_rate_over_interval_mbps": 0.0032 if overall else 0.32,
            "data_tx_opportunity_gap_fraction": 0.99984 if overall else 0.984,
            "data_tx_profile": profiles,
            "mean_dominant_data_phy_rate_mbps": None,
            "mean_effective_phy_rate_mbps": None,
            "aggregate_data_tx_rate_over_interval_mbps": None,
        }
    profiles = [
        {
            "channel_width_mhz": 40,
            "nss": 1,
            "mcs": 4,
            "transmitted_psdu_bytes": 250.0,
            "ppdu_attempt_count": 3,
            "ppdu_airtime_us": 100.0,
        }
    ]
    return {
        "dominant_data_phy_rate_mbps": 87.75,
        "dominant_data_profile_share": 1.0,
        "effective_phy_rate_mbps": 20.0,
        "data_tx_rate_over_interval_mbps": 0.002 if overall else 0.2,
        "data_tx_opportunity_gap_fraction": 0.9999 if overall else 0.99,
        "data_tx_profile": profiles,
        "mean_dominant_data_phy_rate_mbps": None,
        "mean_effective_phy_rate_mbps": None,
        "aggregate_data_tx_rate_over_interval_mbps": None,
    }


def _idle_station_phy() -> dict[str, object]:
    return {
        "dominant_data_phy_rate_mbps": None,
        "dominant_data_profile_share": None,
        "effective_phy_rate_mbps": None,
        "data_tx_rate_over_interval_mbps": 0.0,
        "data_tx_opportunity_gap_fraction": None,
        "data_tx_profile": [],
        "mean_dominant_data_phy_rate_mbps": None,
        "mean_effective_phy_rate_mbps": None,
        "aggregate_data_tx_rate_over_interval_mbps": None,
    }


def _bss_phy(bss_id: int, *, overall: bool) -> dict[str, object]:
    if bss_id == 0:
        dominant, effective = 1020.833334, 20.0
        aggregate = 0.0032 if overall else 0.32
    elif bss_id == 1:
        dominant, effective = 87.75, 20.0
        aggregate = 0.002 if overall else 0.2
    else:
        dominant, effective, aggregate = None, None, 0.0
    return {
        "dominant_data_phy_rate_mbps": None,
        "dominant_data_profile_share": None,
        "effective_phy_rate_mbps": None,
        "data_tx_rate_over_interval_mbps": None,
        "data_tx_opportunity_gap_fraction": None,
        "data_tx_profile": [],
        "mean_dominant_data_phy_rate_mbps": dominant,
        "mean_effective_phy_rate_mbps": effective,
        "aggregate_data_tx_rate_over_interval_mbps": aggregate,
    }


def _ap_identity(bss_id: int) -> dict[str, object]:
    return {
        "access_point_id": bss_id,
        "node_id": 100 + bss_id,
        "node_label": f"AP{bss_id}",
        "ipv4": f"10.{bss_id + 1}.0.1",
    }


def _station_identity(bss_id: int, station_index: int = 0) -> dict[str, object]:
    return {
        "access_point_id": bss_id,
        "station_index": station_index,
        "node_id": 200 + bss_id * 30 + station_index,
        "node_label": f"AP{bss_id}/STA{station_index}",
        "ipv4": f"10.{bss_id + 1}.0.{station_index + 2}",
    }


def _entity(identity: dict[str, object], phy: dict[str, object]) -> dict[str, object]:
    return {**deepcopy(identity), **_categories(phy)}


def make_output_document(
    configuration: ExperimentConfiguration | None = None,
    *,
    repetition_attempt: int = 1,
    repetitions: int = 1,
    run_folder: str = "/tmp/saturated/experiment_012/attempt_1",
) -> tuple[dict[str, object], dict[str, object]]:
    """Build a complete literal three-BSS schema-v2 benchmark fixture."""
    configuration = configuration or make_configuration()
    if configuration.sta_count_per_bss != 1:
        raise ValueError("the independent v2 fixture supports exactly one station per BSS")
    effective = make_effective_configuration(
        configuration,
        repetition_attempt=repetition_attempt,
        repetitions=repetitions,
        run_folder=run_folder,
    )
    window_stations = [
        _entity(_station_identity(0), _active_station_phy(0, overall=False)),
        _entity(_station_identity(1), _active_station_phy(1, overall=False)),
    ]
    window_access_points = [
        _entity(_ap_identity(0), _bss_phy(0, overall=False)),
        _entity(_ap_identity(1), _bss_phy(1, overall=False)),
    ]
    overall_stations = [
        _entity(_station_identity(0), _active_station_phy(0, overall=True)),
        _entity(_station_identity(1), _active_station_phy(1, overall=True)),
        _entity(_station_identity(2), _idle_station_phy()),
    ]
    overall_access_points = [
        _entity(_ap_identity(bss_id), _bss_phy(bss_id, overall=True))
        for bss_id in range(3)
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
                "access_points": [_ap_identity(bss_id) for bss_id in range(3)],
                "stations": [_station_identity(bss_id) for bss_id in range(3)],
            },
        },
    }
    return document, effective


class SaturatedTcpValidationTest(unittest.TestCase):
    """Protect metadata, exact roles, raw profiles, formulas, and row copying."""

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

    def test_valid_document_returns_exact_direct_rows_and_compact_profiles(self) -> None:
        rows = self.validate()
        self.assertEqual(tuple(row.bss_id for row in rows), (0, 1, 2))
        self.assertEqual(tuple(row.configuration for row in rows), (self.configuration,) * 3)
        self.assertEqual(tuple(row.target_rssi_dbm for row in rows), (-50.0,) * 3)
        first = rows[0]
        self.assertEqual(first.mean_dominant_data_phy_rate_mbps, 1020.833334)
        self.assertEqual(first.mean_effective_phy_rate_mbps, 20.0)
        self.assertEqual(first.aggregate_data_tx_rate_over_interval_mbps, 0.0032)
        self.assertIsNone(first.competition_overhead_vs_single_sta)
        self.assertEqual(
            first.stations[0],
            StationCsvMetrics(
                1020.833334,
                0.75,
                20.0,
                0.0032,
                0.99984,
                "W20_NSS1_MCS9:bytes=100,ppdus=2,airtime_us=40|"
                "W80_NSS2_MCS11:bytes=300,ppdus=4,airtime_us=120",
            ),
        )
        self.assertEqual(first.stations[1:], (None,) * 29)
        self.assertEqual(
            rows[2].stations[0],
            StationCsvMetrics(None, None, None, 0.0, None, ""),
        )
        self.assertIsNone(rows[2].mean_dominant_data_phy_rate_mbps)
        self.assertIsNone(rows[2].mean_effective_phy_rate_mbps)
        self.assertEqual(rows[2].aggregate_data_tx_rate_over_interval_mbps, 0.0)

    def test_fully_idle_output_may_have_no_sparse_windows(self) -> None:
        document = deepcopy(self.document)
        document["windows"] = []
        for station in document["overall"]["stations"]:
            station["phy_stats"].update(_idle_station_phy())
        for access_point in document["overall"]["access_points"]:
            access_point["phy_stats"].update(_bss_phy(2, overall=True))

        rows = self.validate(document)

        self.assertEqual(
            tuple(row.aggregate_data_tx_rate_over_interval_mbps for row in rows),
            (0.0, 0.0, 0.0),
        )
        self.assertEqual(
            tuple(row.stations[0] for row in rows),
            (StationCsvMetrics(None, None, None, 0.0, None, ""),) * 3,
        )

    def test_caller_expectations_cannot_override_fixed_scenario_metadata(self) -> None:
        mutations = (
            ("wifi", "guard_interval_ns", 1600),
            ("wifi", "guard_interval_ns", 3200.0),
            ("wifi", "rts_cts_threshold_bytes", 1),
            ("wifi", "rts_cts_threshold_bytes", False),
            ("wifi", "bandwidth_mhz", 40),
            ("tcp", "segment_size_bytes", 1400),
        )
        for section, field, value in mutations:
            with self.subTest(section=section, field=field, value=value):
                document = deepcopy(self.document)
                expected = deepcopy(self.effective)
                document["experiment_metadata"]["configuration"][section][field] = value
                expected[section][field] = value
                with self.assertRaisesRegex(OutputValidationError, f"{section}.*{field}"):
                    validate_output_document(
                        document,
                        self.configuration,
                        repetition_attempt=1,
                        expected_configuration=expected,
                        source_path="fixture.json",
                    )

    def test_default_shared_categories_require_exact_json_scalar_types(self) -> None:
        mutations = []
        floating_counter = deepcopy(self.document)
        floating_counter["overall"]["stations"][0]["general_stats"]["uplink"][
            "estimated_transmitted_tcp_payload_bytes"
        ] = 0.0
        mutations.append(("general_stats", floating_counter))
        integer_airtime = deepcopy(self.document)
        integer_airtime["overall"]["stations"][0]["phy_stats"]["uplink"][
            "transmission_airtime_us"
        ] = 0
        mutations.append(("phy_stats", integer_airtime))

        for expected_path, document in mutations:
            with self.subTest(expected_path=expected_path):
                with self.assertRaisesRegex(OutputValidationError, expected_path):
                    self.validate(document)

    def test_profile_and_station_formulas_are_independently_reconstructed(self) -> None:
        mutations: list[tuple[str, dict[str, object]]] = []
        for field, value in (
            ("dominant_data_phy_rate_mbps", 999.0),
            ("dominant_data_profile_share", 0.5),
            ("effective_phy_rate_mbps", 21.0),
            ("data_tx_rate_over_interval_mbps", 0.004),
            ("data_tx_opportunity_gap_fraction", 0.9),
        ):
            changed = deepcopy(self.document)
            changed["overall"]["stations"][0]["phy_stats"][field] = value
            mutations.append((field, changed))
        changed_bytes = deepcopy(self.document)
        changed_bytes["overall"]["stations"][0]["phy_stats"]["data_tx_profile"][0][
            "transmitted_psdu_bytes"
        ] = 101.0
        mutations.append(("profile|bytes|reproduce", changed_bytes))
        changed_attempt = deepcopy(self.document)
        changed_attempt["overall"]["stations"][0]["phy_stats"]["data_tx_profile"][0][
            "ppdu_attempt_count"
        ] = 3
        mutations.append(("profile|attempt|reproduce", changed_attempt))
        for expected, document in mutations:
            with self.subTest(expected=expected):
                with self.assertRaisesRegex(OutputValidationError, expected):
                    self.validate(document)

    def test_profile_shape_order_ranges_and_old_fields_are_rejected(self) -> None:
        mutations = []
        reordered = deepcopy(self.document)
        profiles = reordered["overall"]["stations"][0]["phy_stats"]["data_tx_profile"]
        profiles[0], profiles[1] = profiles[1], profiles[0]
        mutations.append(("order|profile", reordered))
        duplicate = deepcopy(self.document)
        profiles = duplicate["overall"]["stations"][0]["phy_stats"]["data_tx_profile"]
        profiles[1]["channel_width_mhz"] = 20
        profiles[1]["nss"] = 1
        profiles[1]["mcs"] = 9
        mutations.append(("order|duplic", duplicate))
        for field, value in (
            ("channel_width_mhz", 160),
            ("nss", 0),
            ("nss", 3),
            ("mcs", 12),
            ("transmitted_psdu_bytes", -1.0),
            ("ppdu_attempt_count", 1.0),
            ("ppdu_airtime_us", math.inf),
        ):
            changed = deepcopy(self.document)
            changed["overall"]["stations"][0]["phy_stats"]["data_tx_profile"][0][field] = value
            mutations.append((field, changed))
        old = deepcopy(self.document)
        old["overall"]["stations"][0]["phy_stats"][
            "average_theoretical_phy_rate_mbps"
        ] = 1.0
        mutations.append(("phy_stats|ordered keys", old))
        for expected, document in mutations:
            with self.subTest(expected=expected):
                with self.assertRaisesRegex(OutputValidationError, expected):
                    self.validate(document)

    def test_station_and_bss_role_population_is_exact(self) -> None:
        mutations = []
        station_aggregate = deepcopy(self.document)
        station_aggregate["overall"]["stations"][0]["phy_stats"][
            "aggregate_data_tx_rate_over_interval_mbps"
        ] = 1.0
        mutations.append(("station|BSS", station_aggregate))
        bss_profile = deepcopy(self.document)
        bss_profile["overall"]["access_points"][0]["phy_stats"]["data_tx_profile"] = [
            deepcopy(
                self.document["overall"]["stations"][0]["phy_stats"]["data_tx_profile"][0]
            )
        ]
        mutations.append(("BSS|station", bss_profile))
        partial_idle = deepcopy(self.document)
        partial_idle["overall"]["stations"][2]["phy_stats"][
            "data_tx_rate_over_interval_mbps"
        ] = None
        mutations.append(("zero|idle|shape", partial_idle))
        active_null = deepcopy(self.document)
        active_null["overall"]["stations"][0]["phy_stats"][
            "dominant_data_profile_share"
        ] = None
        mutations.append(("profile|undefined|active", active_null))
        partial_bss = deepcopy(self.document)
        partial_bss["overall"]["access_points"][0]["phy_stats"][
            "mean_effective_phy_rate_mbps"
        ] = None
        mutations.append(("BSS|mean|station-derived", partial_bss))
        for expected, document in mutations:
            with self.subTest(expected=expected):
                with self.assertRaisesRegex(OutputValidationError, expected):
                    self.validate(document)

    def test_bss_means_and_aggregate_are_recomputed_from_station_values(self) -> None:
        for field in (
            "mean_dominant_data_phy_rate_mbps",
            "mean_effective_phy_rate_mbps",
            "aggregate_data_tx_rate_over_interval_mbps",
        ):
            with self.subTest(field=field):
                changed = deepcopy(self.document)
                changed["overall"]["access_points"][0]["phy_stats"][field] += 1.0
                with self.assertRaisesRegex(OutputValidationError, "station-derived|BSS"):
                    self.validate(changed)

    def test_dl_metadata_keeps_station_only_semantics(self) -> None:
        dl_configuration = make_configuration(experiment_id=11, traffic_mode="dl")
        document, expected = make_output_document(dl_configuration)
        self.assertEqual(
            len(
                validate_output_document(
                    document,
                    dl_configuration,
                    repetition_attempt=1,
                    expected_configuration=expected,
                    source_path="dl.json",
                )
            ),
            3,
        )
        document["measurement_semantics"]["phy_observation_scope"] = "AP downlink data PPDUs"
        with self.assertRaisesRegex(OutputValidationError, "measurement_semantics"):
            validate_output_document(
                document,
                dl_configuration,
                repetition_attempt=1,
                expected_configuration=expected,
                source_path="dl.json",
            )

    def test_exact_root_configuration_and_flags_are_required(self) -> None:
        mutations = []
        wrong_schema = deepcopy(self.document)
        wrong_schema["schema_version"] = 1
        mutations.append(("schema_version", wrong_schema))
        reordered_root = {key: self.document[key] for key in reversed(tuple(self.document))}
        mutations.append(("root object", reordered_root))
        wrong_guard = deepcopy(self.document)
        wrong_guard["experiment_metadata"]["configuration"]["wifi"]["guard_interval_ns"] = 1600
        mutations.append(("configuration", wrong_guard))
        wrong_rts = deepcopy(self.document)
        wrong_rts["experiment_metadata"]["configuration"]["wifi"][
            "rts_cts_threshold_bytes"
        ] = 1
        mutations.append(("configuration", wrong_rts))
        false_flag = deepcopy(self.document)
        false_flag["validation"]["overall_matches_windows"] = False
        mutations.append(("overall_matches_windows", false_flag))
        for expected, document in mutations:
            with self.subTest(expected=expected):
                with self.assertRaisesRegex(OutputValidationError, expected):
                    self.validate(document)

    def test_inventory_and_dense_overall_order_are_exact(self) -> None:
        mutations = []
        swapped = deepcopy(self.document)
        swapped["experiment_metadata"]["entity_inventory"]["access_points"][0], swapped[
            "experiment_metadata"
        ]["entity_inventory"]["access_points"][1] = (
            swapped["experiment_metadata"]["entity_inventory"]["access_points"][1],
            swapped["experiment_metadata"]["entity_inventory"]["access_points"][0],
        )
        mutations.append(swapped)
        missing = deepcopy(self.document)
        missing["overall"]["stations"].pop()
        mutations.append(missing)
        wrong_identity = deepcopy(self.document)
        wrong_identity["overall"]["stations"][0]["node_id"] = 999
        mutations.append(wrong_identity)
        for document in mutations:
            with self.assertRaisesRegex(OutputValidationError, "inventory|dense|identity|order"):
                self.validate(document)

    def test_repetitions_and_identity_ids_require_exact_uint32_types(self) -> None:
        maximum = (1 << 32) - 1
        valid = deepcopy(self.document)
        expected = deepcopy(self.effective)
        valid["experiment_metadata"]["configuration"]["script"]["repetitions"] = maximum
        expected["script"]["repetitions"] = maximum
        self.assertEqual(
            len(
                validate_output_document(
                    valid,
                    self.configuration,
                    repetition_attempt=1,
                    expected_configuration=expected,
                    source_path="fixture.json",
                )
            ),
            3,
        )
        for value in (False, 0, -1, 1 << 32):
            with self.subTest(value=value):
                changed = deepcopy(self.document)
                changed["experiment_metadata"]["configuration"]["script"]["repetitions"] = value
                changed_expected = deepcopy(self.effective)
                changed_expected["script"]["repetitions"] = value
                with self.assertRaisesRegex(OutputValidationError, "repetitions.*uint32|uint32.*repetitions"):
                    validate_output_document(
                        changed,
                        self.configuration,
                        repetition_attempt=1,
                        expected_configuration=changed_expected,
                        source_path="fixture.json",
                    )

        floating = deepcopy(self.document)
        floating["overall"]["stations"][0]["station_index"] = 0.0
        with self.assertRaisesRegex(OutputValidationError, "uint32"):
            self.validate(floating)

    def test_unrelated_categories_remain_exact_shared_defaults(self) -> None:
        changed = deepcopy(self.document)
        changed["overall"]["stations"][0]["app_stats"]["uplink"][
            "accepted_send_count"
        ] = 1
        with self.assertRaisesRegex(OutputValidationError, "default"):
            self.validate(changed)

    def test_loader_rejects_duplicate_keys_nonstandard_numbers_and_symlinks(self) -> None:
        with TemporaryDirectory() as directory:
            duplicate = Path(directory) / "duplicate.json"
            duplicate.write_text('{"schema_version":2,"schema_version":2}', encoding="utf-8")
            with self.assertRaisesRegex(OutputValidationError, "duplicate.*schema_version"):
                load_output_document(
                    duplicate,
                    self.configuration,
                    repetition_attempt=1,
                    expected_configuration=self.effective,
                )
            nonfinite = Path(directory) / "nonfinite.json"
            nonfinite.write_text('{"value":NaN}', encoding="utf-8")
            with self.assertRaisesRegex(OutputValidationError, "NaN|non-standard"):
                load_output_document(
                    nonfinite,
                    self.configuration,
                    repetition_attempt=1,
                    expected_configuration=self.effective,
                )

        with TemporaryDirectory() as directory, TemporaryDirectory() as outside:
            outside_path = Path(outside) / "outside.json"
            outside_path.write_text(json.dumps(self.document), encoding="utf-8")
            symlink = Path(directory) / "output.json"
            symlink.symlink_to(outside_path)
            with self.assertRaisesRegex(OutputValidationError, "symlink|regular"):
                load_output_document(
                    symlink,
                    self.configuration,
                    repetition_attempt=1,
                    expected_configuration=self.effective,
                )

    def test_loader_matches_direct_validation_and_leaves_overhead_empty(self) -> None:
        with TemporaryDirectory() as directory:
            output_path = Path(directory) / "output.json"
            output_path.write_text(json.dumps(self.document), encoding="utf-8")
            loaded = load_output_document(
                output_path,
                self.configuration,
                repetition_attempt=1,
                expected_configuration=self.effective,
            )
        self.assertEqual(loaded, self.validate())
        self.assertTrue(all(row.competition_overhead_vs_single_sta is None for row in loaded))


if __name__ == "__main__":
    unittest.main()
