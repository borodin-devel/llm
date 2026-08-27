#!/usr/bin/env python3

import json
import pathlib
import sys


ROOT_KEYS = (
    "schema_version",
    "measurement_semantics",
    "statistics_window_ms",
    "windows",
    "overall",
    "validation",
    "experiment_metadata",
)

STATION_PHY_KEYS = (
    "dominant_data_phy_rate_mbps",
    "dominant_data_profile_share",
    "effective_phy_rate_mbps",
    "data_tx_rate_over_interval_mbps",
    "data_tx_opportunity_gap_fraction",
)

BSS_PHY_KEYS = (
    "mean_dominant_data_phy_rate_mbps",
    "mean_effective_phy_rate_mbps",
    "aggregate_data_tx_rate_over_interval_mbps",
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


def validate_and_cleanup(output_path: pathlib.Path) -> None:
    try:
        document = json.loads(output_path.read_text(encoding="utf-8"))
        assert tuple(document) == ROOT_KEYS
        assert document.get("schema_version") == 2
        assert document.get("statistics_window_ms") == 10
        assert len(document.get("windows")) == 100

        inventory = document.get("experiment_metadata").get("entity_inventory")
        assert len(inventory.get("access_points")) == 3
        assert len(inventory.get("stations")) == 3

        overall = document.get("overall")
        assert len(overall.get("access_points")) == 3
        assert len(overall.get("stations")) == 3
        for entity in overall.get("stations"):
            phy = entity.get("phy_stats")
            assert all(phy.get(key) is not None for key in STATION_PHY_KEYS)
            assert phy.get("data_tx_profile")
            assert all(phy.get(key) is None for key in BSS_PHY_KEYS)
        for entity in overall.get("access_points"):
            phy = entity.get("phy_stats")
            assert all(phy.get(key) is None for key in STATION_PHY_KEYS)
            assert phy.get("data_tx_profile") == []
            assert all(phy.get(key) is not None for key in BSS_PHY_KEYS)

        benchmark = (
            document.get("experiment_metadata").get("configuration").get("benchmark")
        )
        assert benchmark.get("sta_count_per_bss") == 1
        assert benchmark.get("rssi_range") == "high"
        assert benchmark.get("interference_mode") == "isolated"
        assert benchmark.get("traffic_mode") == "ul"
        assert benchmark.get("mimo_mode") == "su"

        validation = document.get("validation")
        assert tuple(validation) == VALIDATION_KEYS
        assert all(validation.values())
    finally:
        output_path.unlink(missing_ok=True)


def main(arguments: list[str]) -> int:
    if len(arguments) != 1:
        raise SystemExit("usage: smoke_json.py OUTPUT_JSON")
    output_path = pathlib.Path(arguments[0])
    validate_and_cleanup(output_path)
    assert not output_path.exists()
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
