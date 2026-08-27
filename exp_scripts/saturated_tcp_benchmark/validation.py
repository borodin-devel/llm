"""Strict shared-schema-v2 and saturated benchmark JSON validation."""

from __future__ import annotations

from dataclasses import dataclass
import ipaddress
import json
import math
import os
from pathlib import Path
import stat
from typing import NoReturn

from .csv_output import BssCsvRow, StationCsvMetrics
from .matrix import (
    INTERFERENCE_MODES,
    MIMO_MODES,
    RSSI_RANGES,
    STA_COUNTS,
    TRAFFIC_MODES,
    ExperimentConfiguration,
    target_rssi_dbm,
)


_ROOT_KEYS = (
    "schema_version",
    "measurement_semantics",
    "statistics_window_ms",
    "windows",
    "overall",
    "validation",
    "experiment_metadata",
)
_MEASUREMENT_SEMANTICS = {
    "access_point_role": "station-derived BSS aggregate",
    "station_role": "per-station transmitted data PPDU detail",
    "parent_child_duplication": "intentional",
    "phy_observation_scope": "qualifying station-transmitted unicast data PPDUs",
    "phy_rate_source": "actual fixed-invariant WifiTxVector NSS and MCS",
    "effective_phy_rate": "transmitted data PSDU bits per data PPDU airtime",
    "data_tx_rate_over_interval": "transmitted data PSDU bits per statistics interval",
    "data_tx_opportunity_gap": "time outside station data PPDU airtime",
    "sparse_window_absence": "zero station data profile activity",
    "undefined_derived_values": None,
}
_CONFIGURATION_KEYS = {
    "general": ("output_name", "run_folder"),
    "script": ("repetitions",),
    "simulation": ("rng_seed", "rng_run"),
    "benchmark": (
        "sta_count_per_bss",
        "rssi_range",
        "interference_mode",
        "traffic_mode",
        "mimo_mode",
    ),
    "wifi": (
        "band",
        "channel_number",
        "bandwidth_mhz",
        "primary_20_index",
        "tx_power_dbm",
        "rate_manager",
        "guard_interval_ns",
        "rts_cts_threshold_bytes",
        "antennas",
        "max_tx_spatial_streams",
        "max_rx_spatial_streams",
    ),
    "tcp": (
        "congestion_control",
        "segment_size_bytes",
        "send_buffer_bytes",
        "receive_buffer_bytes",
        "wired_rate",
        "wired_delay",
    ),
    "statistics": ("window_ms",),
    "logging": ("scenario_level",),
}
_VALIDATION_KEYS = (
    "entity_inventory_references_valid",
    "app_agent_totals_consistent",
    "app_peer_totals_consistent",
    "mac_peer_totals_consistent",
    "phy_peer_totals_consistent",
    "ap_station_sender_totals_consistent",
    "overall_matches_windows",
    "unique_phy_payload_within_tagged_payload",
)
_WINDOW_KEYS = (
    "window_index",
    "window_start_ms",
    "window_duration_ms",
    "access_points",
    "stations",
)
_AP_IDENTITY_KEYS = ("access_point_id", "node_id", "node_label", "ipv4")
_STATION_IDENTITY_KEYS = (
    "access_point_id",
    "station_index",
    "node_id",
    "node_label",
    "ipv4",
)
_CATEGORY_KEYS = ("general_stats", "app_stats", "tcp_stats", "mac_stats", "phy_stats")
_PHY_KEYS = (
    "dominant_data_phy_rate_mbps",
    "dominant_data_profile_share",
    "effective_phy_rate_mbps",
    "data_tx_rate_over_interval_mbps",
    "data_tx_opportunity_gap_fraction",
    "data_tx_profile",
    "mean_dominant_data_phy_rate_mbps",
    "mean_effective_phy_rate_mbps",
    "aggregate_data_tx_rate_over_interval_mbps",
    "busy_time_us",
    "channel_utilization_percent",
    "uplink",
    "downlink",
)
_PROFILE_KEYS = (
    "channel_width_mhz",
    "nss",
    "mcs",
    "transmitted_psdu_bytes",
    "ppdu_attempt_count",
    "ppdu_airtime_us",
)
_METRIC_TOLERANCE = 1e-9
_UINT32_MAX = (1 << 32) - 1


class OutputValidationError(ValueError):
    """A path-bearing rejected benchmark output document."""


@dataclass(frozen=True)
class _ProfileValues:
    width: int
    nss: int
    mcs: int
    transmitted_bytes: float
    attempts: int
    airtime_us: float

    @property
    def key(self) -> tuple[int, int, int]:
        return self.width, self.nss, self.mcs


@dataclass(frozen=True)
class _StationValues:
    dominant: float | None
    share: float | None
    effective: float | None
    interval_rate: float
    gap: float | None
    profiles: tuple[_ProfileValues, ...]
    profile_text: str


@dataclass(frozen=True)
class _BssValues:
    mean_dominant: float | None
    mean_effective: float | None
    aggregate_interval_rate: float


def _fail(source_path: str | Path, json_path: str, message: str) -> NoReturn:
    raise OutputValidationError(f"{source_path}: {json_path}: {message}")


def _expect_keys(
    value: object,
    expected: tuple[str, ...],
    source_path: str | Path,
    json_path: str,
) -> dict[str, object]:
    if type(value) is not dict:
        _fail(source_path, json_path, "expected a JSON object")
    if tuple(value) != expected:
        object_name = "root object" if json_path == "$" else "object"
        _fail(
            source_path,
            json_path,
            f"{object_name} expected exact ordered keys {expected!r}, got {tuple(value)!r}",
        )
    return value


def _expect_list(value: object, source_path: str | Path, json_path: str) -> list[object]:
    if type(value) is not list:
        _fail(source_path, json_path, "expected a JSON array")
    return value


def _is_number(value: object) -> bool:
    return not isinstance(value, bool) and isinstance(value, (int, float))


def _finite_number(value: object, source_path: str | Path, json_path: str) -> float:
    if not _is_number(value):
        _fail(source_path, json_path, "expected a finite representable number")
    try:
        converted = float(value)
    except (OverflowError, ValueError):
        _fail(source_path, json_path, "expected a finite representable number")
    if not math.isfinite(converted):
        _fail(source_path, json_path, "expected a finite representable number")
    return converted


def _optional_nonnegative_number(
    value: object, source_path: str | Path, json_path: str
) -> float | None:
    if value is None:
        return None
    result = _finite_number(value, source_path, json_path)
    if result < 0.0:
        _fail(source_path, json_path, "expected a non-negative number or null")
    return result


def _nonnegative_integer(value: object, source_path: str | Path, json_path: str) -> int:
    if type(value) is not int or value < 0:
        _fail(source_path, json_path, "expected a non-negative integer")
    return value


def _positive_integer(value: object, source_path: str | Path, json_path: str) -> int:
    result = _nonnegative_integer(value, source_path, json_path)
    if result == 0:
        _fail(source_path, json_path, "expected a positive integer")
    return result


def _uint32_integer(
    value: object,
    source_path: str | Path,
    json_path: str,
    *,
    positive: bool = False,
) -> int:
    lower = 1 if positive else 0
    if type(value) is not int or not lower <= value <= _UINT32_MAX:
        _fail(
            source_path,
            json_path,
            f"expected an exact uint32 integer in [{lower}, {_UINT32_MAX}]",
        )
    return value


def _nearly_equal(left: float, right: float) -> bool:
    return abs(left - right) <= _METRIC_TOLERANCE * max(1.0, abs(left), abs(right))


def _json_equal(left: object, right: object, *, strict_scalar_types: bool = False) -> bool:
    if type(left) is dict and type(right) is dict:
        return tuple(left) == tuple(right) and all(
            _json_equal(left[key], right[key], strict_scalar_types=strict_scalar_types)
            for key in left
        )
    if type(left) is list and type(right) is list:
        return len(left) == len(right) and all(
            _json_equal(actual, expected, strict_scalar_types=strict_scalar_types)
            for actual, expected in zip(left, right)
        )
    if strict_scalar_types and type(left) is not type(right):
        return False
    if _is_number(left) and _is_number(right):
        try:
            return math.isfinite(float(left)) and math.isfinite(float(right)) and left == right
        except (OverflowError, ValueError):
            return False
    return type(left) is type(right) and left == right


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


def _default_categories() -> dict[str, object]:
    return {
        "general_stats": {"uplink": _general_direction(), "downlink": _general_direction()},
        "app_stats": {"uplink": _app_direction(), "downlink": _app_direction()},
        "tcp_stats": {"uplink": {"connections": []}, "downlink": {"connections": []}},
        "mac_stats": {"uplink": _mac_direction(), "downlink": _mac_direction()},
    }


def _default_phy_remainder() -> dict[str, object]:
    return {
        "busy_time_us": 0,
        "channel_utilization_percent": None,
        "uplink": _phy_direction(),
        "downlink": _phy_direction(),
    }


def _validate_configuration_shape(
    configuration: object, source_path: str | Path, json_path: str
) -> dict[str, object]:
    result = _expect_keys(configuration, tuple(_CONFIGURATION_KEYS), source_path, json_path)
    for section, keys in _CONFIGURATION_KEYS.items():
        _expect_keys(result[section], keys, source_path, f"{json_path}.{section}")
    return result


def _default_expected_configuration(
    actual: dict[str, object],
    configuration: ExperimentConfiguration,
    repetition_attempt: int,
    source_path: str | Path,
) -> dict[str, object]:
    run_folder = actual["general"]["run_folder"]
    if not isinstance(run_folder, str) or not run_folder:
        _fail(source_path, "$.experiment_metadata.configuration.general.run_folder", "expected nonempty string")
    repetitions = _uint32_integer(
        actual["script"]["repetitions"],
        source_path,
        "$.experiment_metadata.configuration.script.repetitions",
        positive=True,
    )
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


def _validate_configuration_coordinate(
    configuration: ExperimentConfiguration,
    repetition_attempt: int,
    source_path: str | Path,
) -> None:
    if not isinstance(configuration, ExperimentConfiguration):
        _fail(source_path, "$", "configuration must be ExperimentConfiguration")
    _positive_integer(configuration.experiment_id, source_path, "$.expected.experiment_id")
    if configuration.sta_count_per_bss not in STA_COUNTS:
        _fail(source_path, "$.expected.sta_count_per_bss", "not in the fixed matrix")
    for value, choices, path in (
        (configuration.rssi_range, RSSI_RANGES, "rssi_range"),
        (configuration.interference_mode, INTERFERENCE_MODES, "interference_mode"),
        (configuration.traffic_mode, TRAFFIC_MODES, "traffic_mode"),
        (configuration.mimo_mode, MIMO_MODES, "mimo_mode"),
    ):
        if value not in choices:
            _fail(source_path, f"$.expected.{path}", "not in the fixed matrix")
    _positive_integer(repetition_attempt, source_path, "$.expected.repetition_attempt")


def _validate_inventory_identity(
    identity: object,
    expected_keys: tuple[str, ...],
    expected_bss_id: int,
    expected_station_index: int | None,
    source_path: str | Path,
    json_path: str,
    node_ids: set[int],
    addresses: set[str],
) -> dict[str, object]:
    record = _expect_keys(identity, expected_keys, source_path, json_path)
    if _uint32_integer(record["access_point_id"], source_path, f"{json_path}.access_point_id") != expected_bss_id:
        _fail(source_path, json_path, "inventory order or access_point_id is invalid")
    if expected_station_index is not None and _uint32_integer(
        record["station_index"], source_path, f"{json_path}.station_index"
    ) != expected_station_index:
        _fail(source_path, json_path, "inventory station order or index is invalid")
    node_id = _uint32_integer(record["node_id"], source_path, f"{json_path}.node_id")
    if node_id in node_ids:
        _fail(source_path, f"{json_path}.node_id", "duplicate inventory node ID")
    node_ids.add(node_id)
    if not isinstance(record["node_label"], str) or not record["node_label"]:
        _fail(source_path, f"{json_path}.node_label", "expected a nonempty string")
    address = record["ipv4"]
    if not isinstance(address, str):
        _fail(source_path, f"{json_path}.ipv4", "expected an IPv4 string")
    try:
        ipaddress.IPv4Address(address)
    except ipaddress.AddressValueError as error:
        _fail(source_path, f"{json_path}.ipv4", f"invalid IPv4 address: {error}")
    if address in addresses:
        _fail(source_path, f"{json_path}.ipv4", "duplicate inventory IPv4 address")
    addresses.add(address)
    return record


def _validate_inventory(
    metadata: dict[str, object], station_count: int, source_path: str | Path
) -> tuple[dict[int, dict[str, object]], dict[tuple[int, int], dict[str, object]]]:
    inventory = _expect_keys(
        metadata["entity_inventory"],
        ("access_points", "stations"),
        source_path,
        "$.experiment_metadata.entity_inventory",
    )
    aps = _expect_list(inventory["access_points"], source_path, "$.experiment_metadata.entity_inventory.access_points")
    stations = _expect_list(inventory["stations"], source_path, "$.experiment_metadata.entity_inventory.stations")
    if len(aps) != 3 or len(stations) != 3 * station_count:
        _fail(source_path, "$.experiment_metadata.entity_inventory", "inventory is not dense for three BSSs")
    node_ids: set[int] = set()
    addresses: set[str] = set()
    ap_by_id = {
        bss_id: _validate_inventory_identity(
            identity,
            _AP_IDENTITY_KEYS,
            bss_id,
            None,
            source_path,
            f"$.experiment_metadata.entity_inventory.access_points[{bss_id}]",
            node_ids,
            addresses,
        )
        for bss_id, identity in enumerate(aps)
    }
    station_by_key = {}
    position = 0
    for bss_id in range(3):
        for station_index in range(station_count):
            station_by_key[(bss_id, station_index)] = _validate_inventory_identity(
                stations[position],
                _STATION_IDENTITY_KEYS,
                bss_id,
                station_index,
                source_path,
                f"$.experiment_metadata.entity_inventory.stations[{position}]",
                node_ids,
                addresses,
            )
            position += 1
    return ap_by_id, station_by_key


def _validate_entity_shape(
    entity: object,
    identity: dict[str, object],
    identity_keys: tuple[str, ...],
    source_path: str | Path,
    json_path: str,
) -> dict[str, object]:
    record = _expect_keys(entity, identity_keys + _CATEGORY_KEYS, source_path, json_path)
    for key in ("access_point_id", "station_index", "node_id"):
        if key in identity_keys:
            _uint32_integer(record[key], source_path, f"{json_path}.{key}")
    for key in identity_keys:
        if not _json_equal(record[key], identity[key], strict_scalar_types=True):
            _fail(source_path, f"{json_path}.{key}", "entity identity does not match inventory")
    defaults = _default_categories()
    for category in defaults:
        if not _json_equal(record[category], defaults[category]):
            _fail(source_path, f"{json_path}.{category}", "benchmark unrelated category must remain at shared-schema default")
    phy = _expect_keys(record["phy_stats"], _PHY_KEYS, source_path, f"{json_path}.phy_stats")
    remainder = {key: phy[key] for key in _default_phy_remainder()}
    if not _json_equal(remainder, _default_phy_remainder()):
        _fail(source_path, f"{json_path}.phy_stats", "benchmark unrelated PHY fields must remain at shared-schema default")
    return record


def _he_data_rate_mbps(width: int, nss: int, mcs: int) -> float:
    usable = {20: 234, 40: 468, 80: 980}[width]
    modulation_and_code = (
        (1, 1 / 2),
        (2, 1 / 2),
        (2, 3 / 4),
        (4, 1 / 2),
        (4, 3 / 4),
        (6, 2 / 3),
        (6, 3 / 4),
        (6, 5 / 6),
        (8, 3 / 4),
        (8, 5 / 6),
        (10, 3 / 4),
        (10, 5 / 6),
    )
    bits, code = modulation_and_code[mcs]
    single_stream_bps = math.ceil((1e9 / 16000.0) * usable * bits * code)
    return single_stream_bps * nss / 1e6


def _compact_number(value: float) -> str:
    return format(value, ".15g")


def _validate_profiles(
    value: object, source_path: str | Path, json_path: str
) -> tuple[_ProfileValues, ...]:
    profiles = _expect_list(value, source_path, json_path)
    result = []
    previous = None
    for index, profile_value in enumerate(profiles):
        path = f"{json_path}[{index}]"
        profile = _expect_keys(profile_value, _PROFILE_KEYS, source_path, path)
        width = _positive_integer(profile["channel_width_mhz"], source_path, f"{path}.channel_width_mhz")
        if width not in (20, 40, 80):
            _fail(source_path, f"{path}.channel_width_mhz", "expected 20, 40, or 80")
        nss = _positive_integer(profile["nss"], source_path, f"{path}.nss")
        if nss > 2:
            _fail(source_path, f"{path}.nss", "exceeds the fixed two spatial streams")
        mcs = _nonnegative_integer(profile["mcs"], source_path, f"{path}.mcs")
        if mcs > 11:
            _fail(source_path, f"{path}.mcs", "expected MCS in [0, 11]")
        transmitted = _finite_number(profile["transmitted_psdu_bytes"], source_path, f"{path}.transmitted_psdu_bytes")
        attempts = _nonnegative_integer(profile["ppdu_attempt_count"], source_path, f"{path}.ppdu_attempt_count")
        airtime = _finite_number(profile["ppdu_airtime_us"], source_path, f"{path}.ppdu_airtime_us")
        if transmitted < 0.0:
            _fail(source_path, f"{path}.transmitted_psdu_bytes", "must be non-negative")
        if airtime < 0.0:
            _fail(source_path, f"{path}.ppdu_airtime_us", "must be non-negative")
        item = _ProfileValues(width, nss, mcs, transmitted, attempts, airtime)
        if previous is not None and item.key <= previous:
            _fail(source_path, json_path, "profiles are duplicated or out of order")
        previous = item.key
        result.append(item)
    return tuple(result)


def _profile_text(profiles: tuple[_ProfileValues, ...]) -> str:
    return "|".join(
        f"W{profile.width}_NSS{profile.nss}_MCS{profile.mcs}:"
        f"bytes={_compact_number(profile.transmitted_bytes)},"
        f"ppdus={profile.attempts},airtime_us={_compact_number(profile.airtime_us)}"
        for profile in profiles
    )


def _validate_station_phy(
    phy: dict[str, object], interval_duration_ms: float, source_path: str | Path, json_path: str
) -> _StationValues:
    for field in (
        "mean_dominant_data_phy_rate_mbps",
        "mean_effective_phy_rate_mbps",
        "aggregate_data_tx_rate_over_interval_mbps",
    ):
        if phy[field] is not None:
            _fail(source_path, f"{json_path}.{field}", "station contains a BSS aggregate field")
    profiles = _validate_profiles(phy["data_tx_profile"], source_path, f"{json_path}.data_tx_profile")
    dominant = _optional_nonnegative_number(phy["dominant_data_phy_rate_mbps"], source_path, f"{json_path}.dominant_data_phy_rate_mbps")
    share = _optional_nonnegative_number(phy["dominant_data_profile_share"], source_path, f"{json_path}.dominant_data_profile_share")
    effective = _optional_nonnegative_number(phy["effective_phy_rate_mbps"], source_path, f"{json_path}.effective_phy_rate_mbps")
    interval_value = phy["data_tx_rate_over_interval_mbps"]
    gap = _optional_nonnegative_number(phy["data_tx_opportunity_gap_fraction"], source_path, f"{json_path}.data_tx_opportunity_gap_fraction")
    if not profiles:
        if dominant is not None or share is not None or effective is not None or gap is not None:
            _fail(source_path, json_path, "idle station must use the null/null/null/zero/null shape")
        if interval_value is None:
            _fail(source_path, json_path, "idle station must use numeric zero interval rate")
        interval_rate = _finite_number(interval_value, source_path, f"{json_path}.data_tx_rate_over_interval_mbps")
        if interval_rate != 0.0:
            _fail(source_path, json_path, "idle station interval rate must be numeric zero")
        return _StationValues(None, None, None, 0.0, None, (), "")
    if None in (dominant, share, effective, gap) or interval_value is None:
        _fail(source_path, json_path, "active station profile has an undefined derived field")
    interval_rate = _finite_number(interval_value, source_path, f"{json_path}.data_tx_rate_over_interval_mbps")
    total_bytes = sum(profile.transmitted_bytes for profile in profiles)
    total_airtime = sum(profile.airtime_us for profile in profiles)
    if total_bytes <= 0.0 or total_airtime <= 0.0:
        _fail(source_path, f"{json_path}.data_tx_profile", "active profile has non-positive totals")
    interval_us = interval_duration_ms * 1000.0
    if total_airtime > interval_us + _METRIC_TOLERANCE:
        _fail(source_path, f"{json_path}.data_tx_profile", "profile airtime exceeds interval")
    selected = profiles[0]
    selected_rate = _he_data_rate_mbps(*selected.key)
    for profile in profiles[1:]:
        nominal = _he_data_rate_mbps(*profile.key)
        if profile.transmitted_bytes > selected.transmitted_bytes or (
            profile.transmitted_bytes == selected.transmitted_bytes and nominal > selected_rate
        ):
            selected, selected_rate = profile, nominal
    expected = (
        ("dominant_data_phy_rate_mbps", dominant, selected_rate),
        ("dominant_data_profile_share", share, selected.transmitted_bytes / total_bytes),
        ("effective_phy_rate_mbps", effective, total_bytes * 8.0 / total_airtime),
        ("data_tx_rate_over_interval_mbps", interval_rate, total_bytes * 8.0 / interval_us),
        ("data_tx_opportunity_gap_fraction", gap, 1.0 - total_airtime / interval_us),
    )
    for field, observed, wanted in expected:
        if observed is None or not _nearly_equal(observed, wanted):
            _fail(source_path, f"{json_path}.{field}", f"{field} does not reproduce profile values")
    return _StationValues(dominant, share, effective, interval_rate, gap, profiles, _profile_text(profiles))


def _validate_bss_phy(
    phy: dict[str, object], source_path: str | Path, json_path: str
) -> _BssValues:
    for field in (
        "dominant_data_phy_rate_mbps",
        "dominant_data_profile_share",
        "effective_phy_rate_mbps",
        "data_tx_rate_over_interval_mbps",
        "data_tx_opportunity_gap_fraction",
    ):
        if phy[field] is not None:
            _fail(source_path, f"{json_path}.{field}", "BSS contains a station-role field")
    if _expect_list(phy["data_tx_profile"], source_path, f"{json_path}.data_tx_profile"):
        _fail(source_path, f"{json_path}.data_tx_profile", "BSS contains a station profile")
    dominant = _optional_nonnegative_number(phy["mean_dominant_data_phy_rate_mbps"], source_path, f"{json_path}.mean_dominant_data_phy_rate_mbps")
    effective = _optional_nonnegative_number(phy["mean_effective_phy_rate_mbps"], source_path, f"{json_path}.mean_effective_phy_rate_mbps")
    aggregate = _finite_number(phy["aggregate_data_tx_rate_over_interval_mbps"], source_path, f"{json_path}.aggregate_data_tx_rate_over_interval_mbps")
    if aggregate < 0.0:
        _fail(source_path, json_path, "BSS aggregate interval rate must be non-negative")
    if aggregate == 0.0:
        if dominant is not None or effective is not None:
            _fail(source_path, json_path, "idle BSS must have null means")
    elif dominant is None or effective is None or dominant <= 0.0 or effective <= 0.0:
        _fail(source_path, json_path, "active BSS requires both positive means")
    return _BssValues(dominant, effective, aggregate)


def _mean(values: list[float]) -> float | None:
    return sum(values) / len(values) if values else None


def _validate_bss_formula(
    actual: _BssValues,
    stations: list[_StationValues],
    source_path: str | Path,
    json_path: str,
) -> None:
    expected = (
        ("mean_dominant_data_phy_rate_mbps", actual.mean_dominant, _mean([x.dominant for x in stations if x.dominant is not None])),
        ("mean_effective_phy_rate_mbps", actual.mean_effective, _mean([x.effective for x in stations if x.effective is not None])),
        ("aggregate_data_tx_rate_over_interval_mbps", actual.aggregate_interval_rate, sum(x.interval_rate for x in stations)),
    )
    for field, observed, wanted in expected:
        valid = (observed is None and wanted is None) or (
            observed is not None and wanted is not None and _nearly_equal(observed, wanted)
        )
        if not valid:
            _fail(source_path, f"{json_path}.{field}", "BSS value does not match station-derived formula")


def _validate_windows(
    windows_value: object,
    window_width: int,
    ap_inventory: dict[int, dict[str, object]],
    station_inventory: dict[tuple[int, int], dict[str, object]],
    source_path: str | Path,
) -> dict[tuple[int, int], list[tuple[_ProfileValues, ...]]]:
    windows = _expect_list(windows_value, source_path, "$.windows")
    if not windows:
        _fail(source_path, "$.windows", "expected at least one sparse benchmark window")
    profiles_by_station: dict[tuple[int, int], list[tuple[_ProfileValues, ...]]] = {}
    previous_index = -1
    for position, window_value in enumerate(windows):
        window_path = f"$.windows[{position}]"
        window = _expect_keys(window_value, _WINDOW_KEYS, source_path, window_path)
        index = _nonnegative_integer(window["window_index"], source_path, f"{window_path}.window_index")
        if index <= previous_index or index >= 1000 // window_width:
            _fail(source_path, f"{window_path}.window_index", "window index is out of order or range")
        previous_index = index
        start = _finite_number(window["window_start_ms"], source_path, f"{window_path}.window_start_ms")
        duration = _finite_number(window["window_duration_ms"], source_path, f"{window_path}.window_duration_ms")
        if not _nearly_equal(start, index * window_width) or not _nearly_equal(duration, window_width):
            _fail(source_path, window_path, "window position or duration is invalid")
        station_values = _expect_list(window["stations"], source_path, f"{window_path}.stations")
        if not station_values:
            _fail(source_path, f"{window_path}.stations", "sparse window has no station profile activity")
        keys = []
        by_bss: dict[int, list[_StationValues]] = {}
        for station_position, station_value in enumerate(station_values):
            path = f"{window_path}.stations[{station_position}]"
            if type(station_value) is not dict:
                _fail(source_path, path, "expected a station object")
            key = (
                _uint32_integer(station_value.get("access_point_id"), source_path, f"{path}.access_point_id"),
                _uint32_integer(station_value.get("station_index"), source_path, f"{path}.station_index"),
            )
            if key not in station_inventory:
                _fail(source_path, path, "station identity is absent from inventory")
            keys.append(key)
            entity = _validate_entity_shape(station_value, station_inventory[key], _STATION_IDENTITY_KEYS, source_path, path)
            metrics = _validate_station_phy(entity["phy_stats"], duration, source_path, f"{path}.phy_stats")
            if not metrics.profiles:
                _fail(source_path, path, "sparse window contains an inactive station")
            by_bss.setdefault(key[0], []).append(metrics)
            profiles_by_station.setdefault(key, []).append(metrics.profiles)
        if keys != sorted(set(keys)):
            _fail(source_path, f"{window_path}.stations", "station entities are duplicated or out of order")
        access_points = _expect_list(window["access_points"], source_path, f"{window_path}.access_points")
        observed_ids = []
        for ap_position, ap_value in enumerate(access_points):
            path = f"{window_path}.access_points[{ap_position}]"
            if type(ap_value) is not dict:
                _fail(source_path, path, "expected an AP object")
            bss_id = _uint32_integer(ap_value.get("access_point_id"), source_path, f"{path}.access_point_id")
            if bss_id not in ap_inventory:
                _fail(source_path, path, "AP identity is absent from inventory")
            observed_ids.append(bss_id)
            entity = _validate_entity_shape(ap_value, ap_inventory[bss_id], _AP_IDENTITY_KEYS, source_path, path)
            metrics = _validate_bss_phy(entity["phy_stats"], source_path, f"{path}.phy_stats")
            _validate_bss_formula(metrics, by_bss.get(bss_id, []), source_path, f"{path}.phy_stats")
        if observed_ids != sorted(by_bss):
            _fail(source_path, f"{window_path}.access_points", "active BSS parents are missing or out of order")
    return profiles_by_station


def _validate_overall(
    overall_value: object,
    ap_inventory: dict[int, dict[str, object]],
    station_inventory: dict[tuple[int, int], dict[str, object]],
    source_path: str | Path,
) -> tuple[dict[tuple[int, int], _StationValues], dict[int, _BssValues]]:
    overall = _expect_keys(overall_value, ("access_points", "stations"), source_path, "$.overall")
    stations = _expect_list(overall["stations"], source_path, "$.overall.stations")
    aps = _expect_list(overall["access_points"], source_path, "$.overall.access_points")
    if len(stations) != len(station_inventory) or len(aps) != 3:
        _fail(source_path, "$.overall", "overall output is not dense over inventory")
    station_metrics = {}
    by_bss: dict[int, list[_StationValues]] = {bss_id: [] for bss_id in range(3)}
    for position, (key, identity) in enumerate(station_inventory.items()):
        path = f"$.overall.stations[{position}]"
        entity = _validate_entity_shape(stations[position], identity, _STATION_IDENTITY_KEYS, source_path, path)
        metrics = _validate_station_phy(entity["phy_stats"], 1000.0, source_path, f"{path}.phy_stats")
        station_metrics[key] = metrics
        by_bss[key[0]].append(metrics)
    ap_metrics = {}
    for bss_id in range(3):
        path = f"$.overall.access_points[{bss_id}]"
        entity = _validate_entity_shape(aps[bss_id], ap_inventory[bss_id], _AP_IDENTITY_KEYS, source_path, path)
        metrics = _validate_bss_phy(entity["phy_stats"], source_path, f"{path}.phy_stats")
        _validate_bss_formula(metrics, by_bss[bss_id], source_path, f"{path}.phy_stats")
        ap_metrics[bss_id] = metrics
    return station_metrics, ap_metrics


def _validate_overall_profiles(
    overall: dict[tuple[int, int], _StationValues],
    windows: dict[tuple[int, int], list[tuple[_ProfileValues, ...]]],
    source_path: str | Path,
) -> None:
    for key, metrics in overall.items():
        merged: dict[tuple[int, int, int], list[float | int]] = {}
        for profiles in windows.get(key, []):
            for profile in profiles:
                total = merged.setdefault(profile.key, [0.0, 0, 0.0])
                total[0] += profile.transmitted_bytes
                total[1] += profile.attempts
                total[2] += profile.airtime_us
        if tuple(profile.key for profile in metrics.profiles) != tuple(sorted(merged)):
            _fail(source_path, f"$.overall.stations[{key[0]},{key[1]}].phy_stats.data_tx_profile", "overall profile keys do not reproduce sparse windows")
        for profile in metrics.profiles:
            wanted = merged[profile.key]
            if (
                not _nearly_equal(profile.transmitted_bytes, float(wanted[0]))
                or profile.attempts != wanted[1]
                or not _nearly_equal(profile.airtime_us, float(wanted[2]))
            ):
                _fail(source_path, f"$.overall.stations[{key[0]},{key[1]}].phy_stats.data_tx_profile", "overall profile values do not reproduce sparse windows")


def _build_rows(
    configuration: ExperimentConfiguration,
    repetition_attempt: int,
    station_metrics: dict[tuple[int, int], _StationValues],
    ap_metrics: dict[int, _BssValues],
) -> tuple[BssCsvRow, ...]:
    rows = []
    for bss_id in range(3):
        stations: list[StationCsvMetrics | None] = []
        for station_index in range(30):
            if station_index >= configuration.sta_count_per_bss:
                stations.append(None)
                continue
            metrics = station_metrics[(bss_id, station_index)]
            stations.append(
                StationCsvMetrics(
                    metrics.dominant,
                    metrics.share,
                    metrics.effective,
                    metrics.interval_rate,
                    metrics.gap,
                    metrics.profile_text,
                )
            )
        bss = ap_metrics[bss_id]
        rows.append(
            BssCsvRow(
                configuration,
                repetition_attempt,
                target_rssi_dbm(configuration.rssi_range),
                bss_id,
                bss.mean_dominant,
                bss.mean_effective,
                bss.aggregate_interval_rate,
                None,
                tuple(stations),
            )
        )
    return tuple(rows)


def validate_output_document(
    document: object,
    configuration: ExperimentConfiguration,
    repetition_attempt: int,
    expected_configuration: dict[str, object] | None = None,
    source_path: str | Path = "<output document>",
) -> tuple[BssCsvRow, ...]:
    """Validate one complete direct-run JSON and return three overhead-free rows."""
    _validate_configuration_coordinate(configuration, repetition_attempt, source_path)
    root = _expect_keys(document, _ROOT_KEYS, source_path, "$")
    if type(root["schema_version"]) is not int or root["schema_version"] != 2:
        _fail(source_path, "$.schema_version", "expected schema_version 2")
    if not _json_equal(root["measurement_semantics"], _MEASUREMENT_SEMANTICS, strict_scalar_types=True):
        _fail(source_path, "$.measurement_semantics", "does not exactly match saturated station-only semantics")
    metadata = _expect_keys(root["experiment_metadata"], ("configuration", "entity_inventory"), source_path, "$.experiment_metadata")
    actual_configuration = _validate_configuration_shape(metadata["configuration"], source_path, "$.experiment_metadata.configuration")
    if expected_configuration is None:
        expected_configuration = _default_expected_configuration(actual_configuration, configuration, repetition_attempt, source_path)
    else:
        _validate_configuration_shape(expected_configuration, source_path, "$.expected_configuration")
    if not _json_equal(actual_configuration, expected_configuration, strict_scalar_types=True):
        _fail(source_path, "$.experiment_metadata.configuration", "configuration does not exactly match the requested attempt")
    _uint32_integer(actual_configuration["script"]["repetitions"], source_path, "$.experiment_metadata.configuration.script.repetitions", positive=True)
    if actual_configuration["general"]["output_name"] != "output.json" or not isinstance(actual_configuration["general"]["run_folder"], str) or not actual_configuration["general"]["run_folder"]:
        _fail(source_path, "$.experiment_metadata.configuration.general", "expected output.json and a nonempty run folder")
    if actual_configuration["simulation"] != {"rng_seed": 12345, "rng_run": repetition_attempt}:
        _fail(source_path, "$.experiment_metadata.configuration.simulation", "wrong seed or attempt RNG run")
    expected_benchmark = {
        "sta_count_per_bss": configuration.sta_count_per_bss,
        "rssi_range": configuration.rssi_range,
        "interference_mode": configuration.interference_mode,
        "traffic_mode": configuration.traffic_mode,
        "mimo_mode": configuration.mimo_mode,
    }
    if actual_configuration["benchmark"] != expected_benchmark:
        _fail(source_path, "$.experiment_metadata.configuration.benchmark", "wrong matrix metadata")
    window_width = _positive_integer(root["statistics_window_ms"], source_path, "$.statistics_window_ms")
    if 1000 % window_width != 0 or actual_configuration["statistics"]["window_ms"] != window_width:
        _fail(source_path, "$.statistics_window_ms", "does not match a divisor configuration width")
    validation = _expect_keys(root["validation"], _VALIDATION_KEYS, source_path, "$.validation")
    for key in _VALIDATION_KEYS:
        if type(validation[key]) is not bool or not validation[key]:
            _fail(source_path, f"$.validation.{key}", "expected true Boolean")
    ap_inventory, station_inventory = _validate_inventory(metadata, configuration.sta_count_per_bss, source_path)
    window_profiles = _validate_windows(root["windows"], window_width, ap_inventory, station_inventory, source_path)
    station_metrics, ap_metrics = _validate_overall(root["overall"], ap_inventory, station_inventory, source_path)
    _validate_overall_profiles(station_metrics, window_profiles, source_path)
    return _build_rows(configuration, repetition_attempt, station_metrics, ap_metrics)


def load_output_document(
    output_path: str | Path,
    configuration: ExperimentConfiguration,
    repetition_attempt: int,
    expected_configuration: dict[str, object] | None = None,
) -> tuple[BssCsvRow, ...]:
    """Parse one retained JSON without accepting duplicate or nonstandard values."""
    path = Path(output_path)

    def reject_duplicate_keys(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise OutputValidationError(f"{path}: duplicate JSON object key {key!r}")
            result[key] = value
        return result

    def reject_constant(value: str) -> NoReturn:
        raise OutputValidationError(f"{path}: non-standard JSON number {value}")

    descriptor = -1
    try:
        path_status = path.lstat()
        if stat.S_ISLNK(path_status.st_mode):
            raise OutputValidationError(f"{path}: output JSON is a symlink")
        if not stat.S_ISREG(path_status.st_mode):
            raise OutputValidationError(f"{path}: output JSON is not a regular file")
        descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC)
        if not stat.S_ISREG(os.fstat(descriptor).st_mode):
            raise OutputValidationError(f"{path}: output JSON is not a regular file")
        with os.fdopen(descriptor, "r", encoding="utf-8") as input_file:
            descriptor = -1
            document = json.load(input_file, object_pairs_hook=reject_duplicate_keys, parse_constant=reject_constant)
    except OutputValidationError:
        raise
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise OutputValidationError(f"{path}: cannot parse output JSON: {error}") from error
    finally:
        if descriptor >= 0:
            os.close(descriptor)
    return validate_output_document(document, configuration, repetition_attempt, expected_configuration=expected_configuration, source_path=path)
