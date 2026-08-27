#!/usr/bin/env python3

from copy import deepcopy
import json
import pathlib
import tempfile
import unittest

from smoke_json import _bss_metrics, _station_metrics, validate_and_cleanup


def _station_phy(bytes_value, attempts, airtime_us, interval_us):
    return {
        "dominant_data_phy_rate_mbps": 1020.833334,
        "dominant_data_profile_share": 1.0,
        "effective_phy_rate_mbps": bytes_value * 8.0 / airtime_us,
        "data_tx_rate_over_interval_mbps": bytes_value * 8.0 / interval_us,
        "data_tx_opportunity_gap_fraction": 1.0 - airtime_us / interval_us,
        "data_tx_profile": [
            {
                "channel_width_mhz": 80,
                "nss": 2,
                "mcs": 11,
                "transmitted_psdu_bytes": bytes_value,
                "ppdu_attempt_count": attempts,
                "ppdu_airtime_us": airtime_us,
            }
        ],
        "mean_dominant_data_phy_rate_mbps": None,
        "mean_effective_phy_rate_mbps": None,
        "aggregate_data_tx_rate_over_interval_mbps": None,
    }


def _bss_phy(station_phy):
    return {
        "dominant_data_phy_rate_mbps": None,
        "dominant_data_profile_share": None,
        "effective_phy_rate_mbps": None,
        "data_tx_rate_over_interval_mbps": None,
        "data_tx_opportunity_gap_fraction": None,
        "data_tx_profile": [],
        "mean_dominant_data_phy_rate_mbps": station_phy[
            "dominant_data_phy_rate_mbps"
        ],
        "mean_effective_phy_rate_mbps": station_phy["effective_phy_rate_mbps"],
        "aggregate_data_tx_rate_over_interval_mbps": station_phy[
            "data_tx_rate_over_interval_mbps"
        ],
    }


def _document():
    windows = []
    for window_index in range(100):
        stations = []
        access_points = []
        for bss_id in range(3):
            station_phy = _station_phy(4.0, 2, 1.6, 10_000.0)
            stations.append(
                {
                    "access_point_id": bss_id,
                    "station_index": 0,
                    "phy_stats": station_phy,
                }
            )
            access_points.append(
                {"access_point_id": bss_id, "phy_stats": _bss_phy(station_phy)}
            )
        windows.append(
            {
                "window_index": window_index,
                "window_start_ms": window_index * 10.0,
                "window_duration_ms": 10.0,
                "access_points": access_points,
                "stations": stations,
            }
        )

    overall_stations = []
    overall_access_points = []
    for bss_id in range(3):
        station_phy = _station_phy(400.0, 200, 160.0, 1_000_000.0)
        overall_stations.append(
            {
                "access_point_id": bss_id,
                "station_index": 0,
                "phy_stats": station_phy,
            }
        )
        overall_access_points.append(
            {"access_point_id": bss_id, "phy_stats": _bss_phy(station_phy)}
        )
    return {
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
        "windows": windows,
        "overall": {
            "access_points": overall_access_points,
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
                "benchmark": {
                    "sta_count_per_bss": 1,
                    "rssi_range": "high",
                    "interference_mode": "isolated",
                    "traffic_mode": "ul",
                    "mimo_mode": "su",
                },
                "wifi": {
                    "bandwidth_mhz": 80,
                    "guard_interval_ns": 3200,
                    "rts_cts_threshold_bytes": 0,
                    "max_tx_spatial_streams": 2,
                },
            },
            "entity_inventory": {
                "access_points": [{"access_point_id": value} for value in range(3)],
                "stations": [
                    {"access_point_id": value, "station_index": 0}
                    for value in range(3)
                ],
            },
        },
    }


class SmokeJsonCleanupTest(unittest.TestCase):
    def validate(self, document):
        with tempfile.TemporaryDirectory() as temp_directory:
            output_path = pathlib.Path(temp_directory) / "output.json"
            output_path.write_text(json.dumps(document), encoding="utf-8")
            validate_and_cleanup(output_path)
            self.assertFalse(output_path.exists())

    def test_accepts_exact_v2_data_phy_formulas_and_window_merge(self):
        self.validate(_document())

    def test_rejects_station_formula_profile_merge_and_bss_mutations(self):
        mutations = []
        changed = deepcopy(_document())
        changed["overall"]["stations"][0]["phy_stats"][
            "dominant_data_phy_rate_mbps"
        ] = 960.8
        mutations.append(changed)
        changed = deepcopy(_document())
        changed["overall"]["access_points"][0]["phy_stats"][
            "aggregate_data_tx_rate_over_interval_mbps"
        ] = 10.0
        mutations.append(changed)

        for document in mutations:
            with self.subTest():
                with self.assertRaises(AssertionError):
                    self.validate(document)

    def test_rejects_only_overall_profile_window_merge_mismatch(self):
        changed = deepcopy(_document())
        station = changed["overall"]["stations"][0]
        station_phy = station["phy_stats"]
        station_phy["data_tx_profile"][0]["transmitted_psdu_bytes"] = 401.0
        station_phy["data_tx_profile"][0]["ppdu_airtime_us"] = 160.4
        station_phy["data_tx_rate_over_interval_mbps"] = 0.003208
        station_phy["data_tx_opportunity_gap_fraction"] = 0.9998396
        access_point = changed["overall"]["access_points"][0]
        access_point["phy_stats"][
            "aggregate_data_tx_rate_over_interval_mbps"
        ] = 0.003208

        station_metrics, _ = _station_metrics(station, 1_000_000.0)
        _bss_metrics(access_point, [station_metrics])
        with self.assertRaises(AssertionError):
            self.validate(changed)

    def test_rejects_semantics_and_fixed_wifi_invariant_mutations(self):
        changed = deepcopy(_document())
        changed["measurement_semantics"]["phy_observation_scope"] = "all PHY"
        with self.assertRaises(AssertionError):
            self.validate(changed)

        changed = deepcopy(_document())
        changed["measurement_semantics"]["extra"] = "accepted by subset checks"
        with self.assertRaises(AssertionError):
            self.validate(changed)

        changed = deepcopy(_document())
        changed["validation"] = {"arbitrary_all_true": True}
        with self.assertRaises(AssertionError):
            self.validate(changed)

        changed = deepcopy(_document())
        changed["experiment_metadata"]["configuration"]["wifi"][
            "guard_interval_ns"
        ] = 1600
        with self.assertRaises(AssertionError):
            self.validate(changed)

    def test_failed_validation_removes_output(self):
        with tempfile.TemporaryDirectory() as temp_directory:
            output_path = pathlib.Path(temp_directory) / "invalid-output.json"
            output_path.write_text("{}", encoding="utf-8")
            with self.assertRaises(AssertionError):
                validate_and_cleanup(output_path)
            self.assertFalse(output_path.exists())


if __name__ == "__main__":
    unittest.main()
