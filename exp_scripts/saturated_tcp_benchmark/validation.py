"""Strict shared-schema and saturated benchmark JSON validation."""

from __future__ import annotations

from dataclasses import dataclass
import ipaddress
import json
import math
from pathlib import Path
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
    "station_role": "per-station transmitted PPDU detail",
    "parent_child_duplication": "intentional",
    "phy_observation_scope": "qualifying station-transmitted PPDUs",
    "phy_rate_source": "actual WifiTxVector and complete PPDU airtime",
    "phy_practical_rate": "qualifying PSDU bits per complete PPDU airtime",
    "contention_fraction": "unioned station EDCA waiting time per interval",
    "sparse_window_absence": "zero station PPDU and contention activity",
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
    "average_theoretical_phy_rate_mbps",
    "average_practical_phy_rate_mbps",
    "channel_efficiency",
    "contention_fraction",
    "busy_time_us",
    "channel_utilization_percent",
    "uplink",
    "downlink",
)
_METRIC_TOLERANCE = 1e-9


class OutputValidationError(ValueError):
    """A path-bearing rejected benchmark output document."""


@dataclass(frozen=True)
class _MetricValues:
    theoretical: float | None
    practical: float | None
    efficiency: float | None
    contention: float


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
    if not _is_number(value) or not math.isfinite(value):
        _fail(source_path, json_path, "expected a finite number")
    return float(value)


def _nonnegative_integer(value: object, source_path: str | Path, json_path: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        _fail(source_path, json_path, "expected a non-negative integer")
    return value


def _positive_integer(value: object, source_path: str | Path, json_path: str) -> int:
    result = _nonnegative_integer(value, source_path, json_path)
    if result == 0:
        _fail(source_path, json_path, "expected a positive integer")
    return result


def _nearly_equal(left: float, right: float) -> bool:
    scale = max(1.0, abs(left), abs(right))
    return abs(left - right) <= _METRIC_TOLERANCE * scale


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
        return math.isfinite(left) and math.isfinite(right) and left == right
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
        "general_stats": {
            "uplink": _general_direction(),
            "downlink": _general_direction(),
        },
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
        _fail(
            source_path,
            "$.experiment_metadata.configuration.general.run_folder",
            "expected a nonempty run folder string",
        )
    repetitions = _positive_integer(
        actual["script"]["repetitions"],
        source_path,
        "$.experiment_metadata.configuration.script.repetitions",
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
    if configuration.rssi_range not in RSSI_RANGES:
        _fail(source_path, "$.expected.rssi_range", "not in the fixed matrix")
    if configuration.interference_mode not in INTERFERENCE_MODES:
        _fail(source_path, "$.expected.interference_mode", "not in the fixed matrix")
    if configuration.traffic_mode not in TRAFFIC_MODES:
        _fail(source_path, "$.expected.traffic_mode", "not in the fixed matrix")
    if configuration.mimo_mode not in MIMO_MODES:
        _fail(
            source_path,
            "$.expected.mimo_mode",
            f"unsupported benchmark MIMO mode {configuration.mimo_mode!r}",
        )
    _positive_integer(repetition_attempt, source_path, "$.expected.repetition_attempt")


def _validate_inventory_identity(
    identity: object,
    expected_keys: tuple[str, ...],
    expected_bss_id: int,
    expected_station_index: int | None,
    source_path: str | Path,
    json_path: str,
    node_ids: set[int],
    ipv4_addresses: set[str],
) -> dict[str, object]:
    record = _expect_keys(identity, expected_keys, source_path, json_path)
    if record["access_point_id"] != expected_bss_id or isinstance(
        record["access_point_id"], bool
    ):
        _fail(source_path, json_path, "inventory order or access_point_id is invalid")
    if expected_station_index is not None and (
        record["station_index"] != expected_station_index
        or isinstance(record["station_index"], bool)
    ):
        _fail(source_path, json_path, "inventory station order or index is invalid")
    node_id = _nonnegative_integer(record["node_id"], source_path, f"{json_path}.node_id")
    if node_id in node_ids:
        _fail(source_path, f"{json_path}.node_id", "duplicate inventory node ID")
    node_ids.add(node_id)
    if not isinstance(record["node_label"], str) or not record["node_label"]:
        _fail(source_path, f"{json_path}.node_label", "expected a nonempty string")
    ipv4 = record["ipv4"]
    if not isinstance(ipv4, str):
        _fail(source_path, f"{json_path}.ipv4", "expected an IPv4 string")
    try:
        ipaddress.IPv4Address(ipv4)
    except ipaddress.AddressValueError as error:
        _fail(source_path, f"{json_path}.ipv4", f"invalid IPv4 address: {error}")
    if ipv4 in ipv4_addresses:
        _fail(source_path, f"{json_path}.ipv4", "duplicate inventory IPv4 address")
    ipv4_addresses.add(ipv4)
    return record


def _validate_inventory(
    metadata: dict[str, object],
    station_count: int,
    source_path: str | Path,
) -> tuple[dict[int, dict[str, object]], dict[tuple[int, int], dict[str, object]]]:
    inventory = _expect_keys(
        metadata["entity_inventory"],
        ("access_points", "stations"),
        source_path,
        "$.experiment_metadata.entity_inventory",
    )
    access_points = _expect_list(
        inventory["access_points"],
        source_path,
        "$.experiment_metadata.entity_inventory.access_points",
    )
    stations = _expect_list(
        inventory["stations"],
        source_path,
        "$.experiment_metadata.entity_inventory.stations",
    )
    if len(access_points) != 3:
        _fail(
            source_path,
            "$.experiment_metadata.entity_inventory.access_points",
            "benchmark inventory must contain exactly three BSS access points",
        )
    if len(stations) != 3 * station_count:
        _fail(
            source_path,
            "$.experiment_metadata.entity_inventory.stations",
            "station inventory is not dense for three BSSs",
        )

    node_ids: set[int] = set()
    ipv4_addresses: set[str] = set()
    ap_by_id = {}
    for bss_id, identity in enumerate(access_points):
        ap_by_id[bss_id] = _validate_inventory_identity(
            identity,
            _AP_IDENTITY_KEYS,
            bss_id,
            None,
            source_path,
            f"$.experiment_metadata.entity_inventory.access_points[{bss_id}]",
            node_ids,
            ipv4_addresses,
        )

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
                ipv4_addresses,
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
    for key in identity_keys:
        if not _json_equal(record[key], identity[key], strict_scalar_types=True):
            _fail(source_path, f"{json_path}.{key}", "entity identity does not match inventory")

    defaults = _default_categories()
    for category in ("general_stats", "app_stats", "tcp_stats", "mac_stats"):
        if not _json_equal(record[category], defaults[category]):
            _fail(
                source_path,
                f"{json_path}.{category}",
                "benchmark unrelated category must remain at shared-schema default",
            )
    phy = _expect_keys(record["phy_stats"], _PHY_KEYS, source_path, f"{json_path}.phy_stats")
    remainder = {key: phy[key] for key in tuple(_default_phy_remainder())}
    if not _json_equal(remainder, _default_phy_remainder()):
        _fail(
            source_path,
            f"{json_path}.phy_stats",
            "benchmark unrelated PHY fields must remain at shared-schema default",
        )
    return record


def _validate_metric_fields(
    entity: dict[str, object],
    *,
    require_rates: bool,
    source_path: str | Path,
    json_path: str,
) -> _MetricValues:
    phy = entity["phy_stats"]
    theoretical_value = phy["average_theoretical_phy_rate_mbps"]
    practical_value = phy["average_practical_phy_rate_mbps"]
    efficiency_value = phy["channel_efficiency"]
    contention = _finite_number(
        phy["contention_fraction"],
        source_path,
        f"{json_path}.phy_stats.contention_fraction",
    )
    if contention < -_METRIC_TOLERANCE or contention > 1.0 + _METRIC_TOLERANCE:
        _fail(
            source_path,
            f"{json_path}.phy_stats.contention_fraction",
            "contention_fraction is outside [0, 1]",
        )

    if require_rates and (
        theoretical_value is None or practical_value is None or efficiency_value is None
    ):
        _fail(
            source_path,
            f"{json_path}.phy_stats",
            "all four overall station fields must be non-null",
        )

    if theoretical_value is None or practical_value is None:
        if theoretical_value is not None or practical_value is not None:
            _fail(source_path, f"{json_path}.phy_stats", "PHY rate presence differs")
        if require_rates:
            _fail(
                source_path,
                f"{json_path}.phy_stats.average_theoretical_phy_rate_mbps",
                "overall station fields must be non-null",
            )
        if efficiency_value is not None:
            _fail(
                source_path,
                f"{json_path}.phy_stats.channel_efficiency",
                "efficiency exists without rates",
            )
        return _MetricValues(None, None, None, contention)

    theoretical = _finite_number(
        theoretical_value,
        source_path,
        f"{json_path}.phy_stats.average_theoretical_phy_rate_mbps",
    )
    practical = _finite_number(
        practical_value,
        source_path,
        f"{json_path}.phy_stats.average_practical_phy_rate_mbps",
    )
    if theoretical < 0.0:
        _fail(
            source_path,
            f"{json_path}.phy_stats.average_theoretical_phy_rate_mbps",
            "rate must be non-negative",
        )
    if practical < 0.0:
        _fail(
            source_path,
            f"{json_path}.phy_stats.average_practical_phy_rate_mbps",
            "rate must be non-negative",
        )
    if practical - theoretical > _METRIC_TOLERANCE * max(1.0, theoretical, practical):
        _fail(
            source_path,
            f"{json_path}.phy_stats.average_practical_phy_rate_mbps",
            "practical rate exceeds theoretical rate",
        )
    if theoretical == 0.0:
        if practical != 0.0 or efficiency_value is not None or require_rates:
            _fail(
                source_path,
                f"{json_path}.phy_stats.channel_efficiency",
                "zero theoretical rate has invalid or non-null efficiency",
            )
        return _MetricValues(theoretical, practical, None, contention)

    if efficiency_value is None:
        _fail(
            source_path,
            f"{json_path}.phy_stats.channel_efficiency",
            "efficiency must be non-null when rates are defined",
        )
    efficiency = _finite_number(
        efficiency_value,
        source_path,
        f"{json_path}.phy_stats.channel_efficiency",
    )
    if efficiency < -_METRIC_TOLERANCE or efficiency > 1.0 + _METRIC_TOLERANCE:
        _fail(
            source_path,
            f"{json_path}.phy_stats.channel_efficiency",
            "channel_efficiency is outside [0, 1]",
        )
    if not _nearly_equal(efficiency, practical / theoretical):
        _fail(
            source_path,
            f"{json_path}.phy_stats.channel_efficiency",
            "channel_efficiency does not equal practical/theoretical",
        )
    return _MetricValues(theoretical, practical, efficiency, contention)


def _mean(values: list[float]) -> float | None:
    return sum(values) / len(values) if values else None


def _validate_ap_formula(
    actual: _MetricValues,
    stations: list[_MetricValues],
    station_count: int,
    source_path: str | Path,
    json_path: str,
) -> None:
    expected_theoretical = _mean(
        [metric.theoretical for metric in stations if metric.theoretical is not None]
    )
    expected_practical = _mean(
        [metric.practical for metric in stations if metric.practical is not None]
    )
    expected_efficiency = None
    if (
        expected_theoretical is not None
        and expected_practical is not None
        and expected_theoretical > 0
    ):
        expected_efficiency = expected_practical / expected_theoretical
    expected_contention = sum(metric.contention for metric in stations) / station_count
    comparisons = (
        ("average_theoretical_phy_rate_mbps", actual.theoretical, expected_theoretical),
        ("average_practical_phy_rate_mbps", actual.practical, expected_practical),
        ("channel_efficiency", actual.efficiency, expected_efficiency),
        ("contention_fraction", actual.contention, expected_contention),
    )
    for field, observed, expected in comparisons:
        if observed is None or expected is None:
            valid = observed is None and expected is None
        else:
            valid = _nearly_equal(observed, expected)
        if not valid:
            _fail(
                source_path,
                f"{json_path}.phy_stats.{field}",
                "AP field does not match station-derived BSS formula",
            )


def _validate_windows(
    windows_value: object,
    window_width: int,
    station_count: int,
    ap_inventory: dict[int, dict[str, object]],
    station_inventory: dict[tuple[int, int], dict[str, object]],
    source_path: str | Path,
) -> dict[tuple[int, int], list[tuple[float, _MetricValues]]]:
    windows = _expect_list(windows_value, source_path, "$.windows")
    if not windows:
        _fail(source_path, "$.windows", "expected at least one sparse benchmark window")
    metrics_by_station: dict[tuple[int, int], list[tuple[float, _MetricValues]]] = {}
    previous_index = -1
    for position, window_value in enumerate(windows):
        window_path = f"$.windows[{position}]"
        window = _expect_keys(window_value, _WINDOW_KEYS, source_path, window_path)
        index = _nonnegative_integer(
            window["window_index"], source_path, f"{window_path}.window_index"
        )
        if index <= previous_index or index >= 1000 // window_width:
            _fail(
                source_path,
                f"{window_path}.window_index",
                "window index is out of order or range",
            )
        previous_index = index
        start = _finite_number(
            window["window_start_ms"], source_path, f"{window_path}.window_start_ms"
        )
        duration = _finite_number(
            window["window_duration_ms"], source_path, f"{window_path}.window_duration_ms"
        )
        if not _nearly_equal(start, index * window_width):
            _fail(source_path, f"{window_path}.window_start_ms", "does not match window index")
        if not _nearly_equal(duration, window_width):
            _fail(
                source_path,
                f"{window_path}.window_duration_ms",
                "does not match configured width",
            )

        stations = _expect_list(window["stations"], source_path, f"{window_path}.stations")
        access_points = _expect_list(
            window["access_points"], source_path, f"{window_path}.access_points"
        )
        if not stations:
            _fail(source_path, f"{window_path}.stations", "sparse window has no station activity")
        station_keys = []
        metrics_by_bss: dict[int, list[_MetricValues]] = {}
        for station_position, station_value in enumerate(stations):
            station_path = f"{window_path}.stations[{station_position}]"
            if type(station_value) is not dict:
                _fail(source_path, station_path, "expected a station object")
            key = (station_value.get("access_point_id"), station_value.get("station_index"))
            if key not in station_inventory:
                _fail(source_path, station_path, "station identity is absent from inventory")
            station_keys.append(key)
            entity = _validate_entity_shape(
                station_value,
                station_inventory[key],
                _STATION_IDENTITY_KEYS,
                source_path,
                station_path,
            )
            metrics = _validate_metric_fields(
                entity,
                require_rates=False,
                source_path=source_path,
                json_path=station_path,
            )
            metrics_by_bss.setdefault(key[0], []).append(metrics)
            metrics_by_station.setdefault(key, []).append((duration, metrics))
        if station_keys != sorted(set(station_keys)):
            _fail(
                source_path,
                f"{window_path}.stations",
                "station entities are duplicated or out of order",
            )

        expected_ap_ids = sorted(metrics_by_bss)
        observed_ap_ids = []
        for ap_position, ap_value in enumerate(access_points):
            ap_path = f"{window_path}.access_points[{ap_position}]"
            if type(ap_value) is not dict or ap_value.get("access_point_id") not in ap_inventory:
                _fail(source_path, ap_path, "AP identity is absent from inventory")
            bss_id = ap_value["access_point_id"]
            observed_ap_ids.append(bss_id)
            entity = _validate_entity_shape(
                ap_value,
                ap_inventory[bss_id],
                _AP_IDENTITY_KEYS,
                source_path,
                ap_path,
            )
            metrics = _validate_metric_fields(
                entity,
                require_rates=False,
                source_path=source_path,
                json_path=ap_path,
            )
            _validate_ap_formula(
                metrics,
                metrics_by_bss[bss_id],
                station_count,
                source_path,
                ap_path,
            )
        if observed_ap_ids != expected_ap_ids:
            _fail(
                source_path,
                f"{window_path}.access_points",
                "active BSS parents are missing or out of order",
            )
    return metrics_by_station


def _validate_overall(
    overall_value: object,
    station_count: int,
    ap_inventory: dict[int, dict[str, object]],
    station_inventory: dict[tuple[int, int], dict[str, object]],
    source_path: str | Path,
) -> tuple[dict[tuple[int, int], _MetricValues], dict[int, _MetricValues]]:
    overall = _expect_keys(overall_value, ("access_points", "stations"), source_path, "$.overall")
    stations = _expect_list(overall["stations"], source_path, "$.overall.stations")
    access_points = _expect_list(overall["access_points"], source_path, "$.overall.access_points")
    if len(stations) != len(station_inventory):
        _fail(
            source_path,
            "$.overall.stations",
            "overall station array is not dense over inventory",
        )
    if len(access_points) != 3:
        _fail(
            source_path,
            "$.overall.access_points",
            "overall AP array is not dense over three BSSs",
        )

    station_metrics = {}
    metrics_by_bss: dict[int, list[_MetricValues]] = {bss_id: [] for bss_id in range(3)}
    for position, (key, identity) in enumerate(station_inventory.items()):
        station_path = f"$.overall.stations[{position}]"
        entity = _validate_entity_shape(
            stations[position], identity, _STATION_IDENTITY_KEYS, source_path, station_path
        )
        metrics = _validate_metric_fields(
            entity,
            require_rates=True,
            source_path=source_path,
            json_path=station_path,
        )
        station_metrics[key] = metrics
        metrics_by_bss[key[0]].append(metrics)

    ap_metrics = {}
    for bss_id in range(3):
        ap_path = f"$.overall.access_points[{bss_id}]"
        entity = _validate_entity_shape(
            access_points[bss_id],
            ap_inventory[bss_id],
            _AP_IDENTITY_KEYS,
            source_path,
            ap_path,
        )
        metrics = _validate_metric_fields(
            entity,
            require_rates=True,
            source_path=source_path,
            json_path=ap_path,
        )
        _validate_ap_formula(
            metrics,
            metrics_by_bss[bss_id],
            station_count,
            source_path,
            ap_path,
        )
        ap_metrics[bss_id] = metrics
    return station_metrics, ap_metrics


def _validate_overall_against_windows(
    overall: dict[tuple[int, int], _MetricValues],
    windows: dict[tuple[int, int], list[tuple[float, _MetricValues]]],
    source_path: str | Path,
) -> None:
    for key, overall_metrics in overall.items():
        observed_windows = windows.get(key)
        path = f"$.overall.stations[{key[0]},{key[1]}]"
        if not observed_windows:
            _fail(source_path, path, "nonzero overall station is absent from sparse windows")
        rate_windows = [
            metrics for _, metrics in observed_windows if metrics.theoretical is not None
        ]
        if not rate_windows:
            _fail(source_path, path, "overall rates have no non-null window observation")
        theoretical_values = [metrics.theoretical for metrics in rate_windows]
        practical_values = [metrics.practical for metrics in rate_windows]
        if (
            overall_metrics.theoretical < min(theoretical_values) - _METRIC_TOLERANCE
            or overall_metrics.theoretical > max(theoretical_values) + _METRIC_TOLERANCE
        ):
            _fail(source_path, path, "overall theoretical rate is outside its window range")
        if (
            overall_metrics.practical < min(practical_values) - _METRIC_TOLERANCE
            or overall_metrics.practical > max(practical_values) + _METRIC_TOLERANCE
        ):
            _fail(source_path, path, "overall practical rate is outside its window range")
        expected_contention = sum(
            metrics.contention * duration_ms / 1000.0
            for duration_ms, metrics in observed_windows
        )
        if not _nearly_equal(overall_metrics.contention, expected_contention):
            _fail(source_path, path, "overall contention does not reproduce sparse windows")


def _build_rows(
    configuration: ExperimentConfiguration,
    repetition_attempt: int,
    station_metrics: dict[tuple[int, int], _MetricValues],
    ap_metrics: dict[int, _MetricValues],
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
                    average_theoretical_phy_rate_mbps=metrics.theoretical,
                    average_practical_phy_rate_mbps=metrics.practical,
                    efficiency=metrics.efficiency,
                    contention_fraction=metrics.contention,
                )
            )
        bss = ap_metrics[bss_id]
        rows.append(
            BssCsvRow(
                configuration=configuration,
                repetition_attempt=repetition_attempt,
                target_rssi_dbm=target_rssi_dbm(configuration.rssi_range),
                bss_id=bss_id,
                average_theoretical_phy_rate_mbps=bss.theoretical,
                average_practical_phy_rate_mbps=bss.practical,
                efficiency=bss.efficiency,
                contention_fraction=bss.contention,
                stations=tuple(stations),
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
    """Validate one complete JSON document and return its three BSS rows."""
    _validate_configuration_coordinate(configuration, repetition_attempt, source_path)
    root = _expect_keys(document, _ROOT_KEYS, source_path, "$")
    if type(root["schema_version"]) is not int or root["schema_version"] != 1:
        _fail(source_path, "$.schema_version", "expected schema_version 1")
    if not _json_equal(
        root["measurement_semantics"],
        _MEASUREMENT_SEMANTICS,
        strict_scalar_types=True,
    ):
        _fail(source_path, "$.measurement_semantics", "does not exactly match saturated semantics")

    metadata = _expect_keys(
        root["experiment_metadata"],
        ("configuration", "entity_inventory"),
        source_path,
        "$.experiment_metadata",
    )
    actual_configuration = _validate_configuration_shape(
        metadata["configuration"], source_path, "$.experiment_metadata.configuration"
    )
    if expected_configuration is None:
        expected_configuration = _default_expected_configuration(
            actual_configuration, configuration, repetition_attempt, source_path
        )
    else:
        _validate_configuration_shape(
            expected_configuration, source_path, "$.expected_configuration"
        )
    if not _json_equal(
        actual_configuration, expected_configuration, strict_scalar_types=True
    ):
        _fail(
            source_path,
            "$.experiment_metadata.configuration",
            "configuration does not exactly match the requested attempt",
        )
    if actual_configuration["general"]["output_name"] != "output.json":
        _fail(
            source_path,
            "$.experiment_metadata.configuration.general.output_name",
            "expected output.json",
        )
    if (
        not isinstance(actual_configuration["general"]["run_folder"], str)
        or not actual_configuration["general"]["run_folder"]
    ):
        _fail(
            source_path,
            "$.experiment_metadata.configuration.general.run_folder",
            "expected nonempty string",
        )
    _positive_integer(
        actual_configuration["script"]["repetitions"],
        source_path,
        "$.experiment_metadata.configuration.script.repetitions",
    )
    if actual_configuration["simulation"] != {"rng_seed": 12345, "rng_run": repetition_attempt}:
        _fail(
            source_path,
            "$.experiment_metadata.configuration.simulation",
            "wrong seed or attempt RNG run",
        )
    expected_benchmark = {
        "sta_count_per_bss": configuration.sta_count_per_bss,
        "rssi_range": configuration.rssi_range,
        "interference_mode": configuration.interference_mode,
        "traffic_mode": configuration.traffic_mode,
        "mimo_mode": configuration.mimo_mode,
    }
    if actual_configuration["benchmark"] != expected_benchmark:
        _fail(source_path, "$.experiment_metadata.configuration.benchmark", "wrong matrix metadata")

    window_width = _positive_integer(
        root["statistics_window_ms"], source_path, "$.statistics_window_ms"
    )
    if 1000 % window_width != 0 or actual_configuration["statistics"]["window_ms"] != window_width:
        _fail(source_path, "$.statistics_window_ms", "does not match a divisor configuration width")

    validation = _expect_keys(root["validation"], _VALIDATION_KEYS, source_path, "$.validation")
    for key in _VALIDATION_KEYS:
        if type(validation[key]) is not bool or not validation[key]:
            _fail(source_path, f"$.validation.{key}", "expected true Boolean")

    ap_inventory, station_inventory = _validate_inventory(
        metadata, configuration.sta_count_per_bss, source_path
    )
    window_metrics = _validate_windows(
        root["windows"],
        window_width,
        configuration.sta_count_per_bss,
        ap_inventory,
        station_inventory,
        source_path,
    )
    station_metrics, ap_metrics = _validate_overall(
        root["overall"],
        configuration.sta_count_per_bss,
        ap_inventory,
        station_inventory,
        source_path,
    )
    _validate_overall_against_windows(station_metrics, window_metrics, source_path)
    return _build_rows(configuration, repetition_attempt, station_metrics, ap_metrics)


def load_output_document(
    output_path: str | Path,
    configuration: ExperimentConfiguration,
    repetition_attempt: int,
    expected_configuration: dict[str, object] | None = None,
) -> tuple[BssCsvRow, ...]:
    """Parse one retained JSON file without accepting duplicate or nonstandard values."""
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

    try:
        with path.open("r", encoding="utf-8") as input_file:
            document = json.load(
                input_file,
                object_pairs_hook=reject_duplicate_keys,
                parse_constant=reject_constant,
            )
    except OutputValidationError:
        raise
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise OutputValidationError(f"{path}: cannot parse output JSON: {error}") from error
    return validate_output_document(
        document,
        configuration,
        repetition_attempt,
        expected_configuration=expected_configuration,
        source_path=path,
    )
