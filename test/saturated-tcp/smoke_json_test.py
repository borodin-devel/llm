#!/usr/bin/env python3

import json
import pathlib
import tempfile
import unittest

from smoke_json import validate_and_cleanup


class SmokeJsonCleanupTest(unittest.TestCase):
    def test_accepts_v2_station_and_bss_data_phy_roles(self):
        station_phy = {
            "dominant_data_phy_rate_mbps": 960.8,
            "dominant_data_profile_share": 1.0,
            "effective_phy_rate_mbps": 720.6,
            "data_tx_rate_over_interval_mbps": 180.15,
            "data_tx_opportunity_gap_fraction": 0.75,
            "data_tx_profile": [{
                "channel_width_mhz": 80,
                "nss": 2,
                "mcs": 11,
                "transmitted_psdu_bytes": 100500.0,
                "ppdu_attempt_count": 120,
                "ppdu_airtime_us": 900.0,
            }],
            "mean_dominant_data_phy_rate_mbps": None,
            "mean_effective_phy_rate_mbps": None,
            "aggregate_data_tx_rate_over_interval_mbps": None,
        }
        bss_phy = {
            "dominant_data_phy_rate_mbps": None,
            "dominant_data_profile_share": None,
            "effective_phy_rate_mbps": None,
            "data_tx_rate_over_interval_mbps": None,
            "data_tx_opportunity_gap_fraction": None,
            "data_tx_profile": [],
            "mean_dominant_data_phy_rate_mbps": 960.8,
            "mean_effective_phy_rate_mbps": 720.6,
            "aggregate_data_tx_rate_over_interval_mbps": 180.15,
        }
        window = {
            "access_points": [{"phy_stats": bss_phy}],
            "stations": [{"phy_stats": station_phy}],
        }
        document = {
            "schema_version": 2,
            "measurement_semantics": {},
            "statistics_window_ms": 10,
            "windows": [window] * 100,
            "overall": {
                "access_points": [{"phy_stats": bss_phy}] * 3,
                "stations": [{"phy_stats": station_phy}] * 3,
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
                "configuration": {"benchmark": {
                    "sta_count_per_bss": 1,
                    "rssi_range": "high",
                    "interference_mode": "isolated",
                    "traffic_mode": "ul",
                    "mimo_mode": "su",
                }},
                "entity_inventory": {
                    "access_points": [{}, {}, {}],
                    "stations": [{}, {}, {}],
                },
            },
        }
        with tempfile.TemporaryDirectory() as temp_directory:
            output_path = pathlib.Path(temp_directory) / "valid-output.json"
            output_path.write_text(json.dumps(document), encoding="utf-8")
            validate_and_cleanup(output_path)
            self.assertFalse(output_path.exists())

    def test_failed_validation_removes_output(self):
        with tempfile.TemporaryDirectory() as temp_directory:
            output_path = pathlib.Path(temp_directory) / "invalid-output.json"
            output_path.write_text("{}", encoding="utf-8")

            with self.assertRaises(AssertionError):
                validate_and_cleanup(output_path)

            self.assertFalse(output_path.exists())


if __name__ == "__main__":
    unittest.main()
