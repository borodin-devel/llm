#!/usr/bin/env python3

import json
import math
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

PROFILE_KEYS = (
    "channel_width_mhz",
    "nss",
    "mcs",
    "transmitted_psdu_bytes",
    "ppdu_attempt_count",
    "ppdu_airtime_us",
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

REQUIRED_SEMANTICS = {
    "access_point_role": "station-derived BSS aggregate",
    "station_role": "per-station transmitted data PPDU detail",
    "phy_observation_scope": "qualifying station-transmitted unicast data PPDUs",
    "effective_phy_rate": "transmitted data PSDU bits per data PPDU airtime",
    "data_tx_rate_over_interval": "transmitted data PSDU bits per statistics interval",
    "data_tx_opportunity_gap": "time outside station data PPDU airtime",
}


def _near(left, right):
    return abs(left - right) <= 1e-9 * max(1.0, abs(left), abs(right))


def _number(value, minimum=0.0):
    assert not isinstance(value, bool)
    assert isinstance(value, (int, float))
    assert math.isfinite(value)
    assert value >= minimum
    return float(value)


def _he_gi3200_rate(width, nss, mcs):
    usable = {20: 234, 40: 468, 80: 980}[width]
    bits = (1, 2, 2, 4, 4, 6, 6, 6, 8, 8, 10, 10)[mcs]
    numerators = (1, 1, 3, 1, 3, 2, 3, 5, 3, 5, 3, 5)
    denominators = (2, 2, 4, 2, 4, 3, 4, 6, 4, 6, 4, 6)
    single_stream_bps = math.ceil(
        usable * bits * numerators[mcs] / denominators[mcs] * 1e9 / 16000.0
    )
    return single_stream_bps * nss / 1e6


def _station_metrics(entity, interval_us):
    phy = entity["phy_stats"]
    assert all(phy.get(key) is None for key in BSS_PHY_KEYS)
    profiles = phy["data_tx_profile"]
    assert isinstance(profiles, list) and profiles
    parsed = []
    previous = None
    for profile in profiles:
        assert tuple(profile) == PROFILE_KEYS
        width = profile["channel_width_mhz"]
        nss = profile["nss"]
        mcs = profile["mcs"]
        assert type(width) is int and width in (20, 40, 80)
        assert type(nss) is int and 1 <= nss <= 2
        assert type(mcs) is int and 0 <= mcs <= 11
        key = width, nss, mcs
        assert previous is None or key > previous
        previous = key
        transmitted = _number(profile["transmitted_psdu_bytes"])
        attempts = profile["ppdu_attempt_count"]
        assert type(attempts) is int and attempts >= 0
        airtime = _number(profile["ppdu_airtime_us"])
        parsed.append((key, transmitted, attempts, airtime))
    total_bytes = sum(value[1] for value in parsed)
    total_airtime = sum(value[3] for value in parsed)
    assert total_bytes > 0.0 and 0.0 < total_airtime <= interval_us
    dominant = max(
        parsed,
        key=lambda value: (
            value[1],
            _he_gi3200_rate(*value[0]),
            -value[0][0],
            -value[0][1],
            -value[0][2],
        ),
    )
    expected = {
        "dominant_data_phy_rate_mbps": _he_gi3200_rate(*dominant[0]),
        "dominant_data_profile_share": dominant[1] / total_bytes,
        "effective_phy_rate_mbps": total_bytes * 8.0 / total_airtime,
        "data_tx_rate_over_interval_mbps": total_bytes * 8.0 / interval_us,
        "data_tx_opportunity_gap_fraction": 1.0 - total_airtime / interval_us,
    }
    for key, wanted in expected.items():
        assert _near(_number(phy[key]), wanted)
    return expected, parsed


def _bss_metrics(entity, stations):
    phy = entity["phy_stats"]
    assert all(phy.get(key) is None for key in STATION_PHY_KEYS)
    assert phy.get("data_tx_profile") == []
    mean_dominant = sum(item["dominant_data_phy_rate_mbps"] for item in stations) / len(
        stations
    )
    mean_effective = sum(item["effective_phy_rate_mbps"] for item in stations) / len(
        stations
    )
    aggregate = sum(item["data_tx_rate_over_interval_mbps"] for item in stations)
    assert _near(_number(phy["mean_dominant_data_phy_rate_mbps"]), mean_dominant)
    assert _near(_number(phy["mean_effective_phy_rate_mbps"]), mean_effective)
    assert _near(
        _number(phy["aggregate_data_tx_rate_over_interval_mbps"]), aggregate
    )


def validate_and_cleanup(output_path: pathlib.Path) -> None:
    try:
        document = json.loads(output_path.read_text(encoding="utf-8"))
        assert tuple(document) == ROOT_KEYS
        assert document.get("schema_version") == 2
        assert document.get("statistics_window_ms") == 10
        semantics = document.get("measurement_semantics")
        assert isinstance(semantics, dict)
        assert all(semantics.get(key) == value for key, value in REQUIRED_SEMANTICS.items())

        metadata = document.get("experiment_metadata")
        inventory = metadata.get("entity_inventory")
        assert len(inventory.get("access_points")) == 3
        assert len(inventory.get("stations")) == 3
        configuration = metadata.get("configuration")
        benchmark = configuration.get("benchmark")
        assert benchmark == {
            "sta_count_per_bss": 1,
            "rssi_range": "high",
            "interference_mode": "isolated",
            "traffic_mode": "ul",
            "mimo_mode": "su",
        }
        wifi = configuration.get("wifi")
        assert wifi.get("bandwidth_mhz") == 80
        assert wifi.get("guard_interval_ns") == 3200
        assert wifi.get("rts_cts_threshold_bytes") == 0
        assert wifi.get("max_tx_spatial_streams") == 2

        merged = {(bss_id, 0): {} for bss_id in range(3)}
        windows = document.get("windows")
        assert len(windows) == 100
        for expected_index, window in enumerate(windows):
            assert window.get("window_index") == expected_index
            assert _near(window.get("window_start_ms"), expected_index * 10.0)
            assert _near(window.get("window_duration_ms"), 10.0)
            stations = window.get("stations")
            access_points = window.get("access_points")
            assert [
                (item.get("access_point_id"), item.get("station_index"))
                for item in stations
            ] == [(0, 0), (1, 0), (2, 0)]
            assert [item.get("access_point_id") for item in access_points] == [0, 1, 2]
            by_bss = {}
            for station in stations:
                key = station["access_point_id"], station["station_index"]
                metrics, profiles = _station_metrics(station, 10_000.0)
                by_bss.setdefault(key[0], []).append(metrics)
                for profile_key, transmitted, attempts, airtime in profiles:
                    total = merged[key].setdefault(profile_key, [0.0, 0, 0.0])
                    total[0] += transmitted
                    total[1] += attempts
                    total[2] += airtime
            for access_point in access_points:
                _bss_metrics(
                    access_point, by_bss[access_point["access_point_id"]]
                )

        overall = document.get("overall")
        stations = overall.get("stations")
        access_points = overall.get("access_points")
        assert [
            (item.get("access_point_id"), item.get("station_index")) for item in stations
        ] == [(0, 0), (1, 0), (2, 0)]
        assert [item.get("access_point_id") for item in access_points] == [0, 1, 2]
        by_bss = {}
        for station in stations:
            key = station["access_point_id"], station["station_index"]
            metrics, profiles = _station_metrics(station, 1_000_000.0)
            by_bss.setdefault(key[0], []).append(metrics)
            expected_profiles = merged[key]
            assert tuple(value[0] for value in profiles) == tuple(sorted(expected_profiles))
            for profile_key, transmitted, attempts, airtime in profiles:
                wanted = expected_profiles[profile_key]
                assert _near(transmitted, wanted[0])
                assert attempts == wanted[1]
                assert _near(airtime, wanted[2])
        for access_point in access_points:
            _bss_metrics(access_point, by_bss[access_point["access_point_id"]])

        validation = document.get("validation")
        assert tuple(validation) == VALIDATION_KEYS
        assert all(value is True for value in validation.values())
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
