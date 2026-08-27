"""Independent read-only audit of retained saturated TCP benchmark runs."""

from __future__ import annotations

from dataclasses import dataclass
import csv
from io import StringIO
import json
import math
import os
from pathlib import Path
import stat
from typing import NoReturn

from .matrix import ExperimentConfiguration, build_matrix


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
    "phy_rate_source": (
        "actual WifiTxVector channel width, NSS, and MCS with fixed "
        "HE SU/GI 3200 ns invariants"
    ),
    "effective_phy_rate": "transmitted data PSDU bits per data PPDU airtime",
    "data_tx_rate_over_interval": "transmitted data PSDU bits per statistics interval",
    "data_tx_opportunity_gap": "time outside station data PPDU airtime",
    "sparse_window_absence": "zero station data profile activity",
    "undefined_derived_values": None,
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
_PROFILE_KEYS = (
    "channel_width_mhz",
    "nss",
    "mcs",
    "transmitted_psdu_bytes",
    "ppdu_attempt_count",
    "ppdu_airtime_us",
)
_STATION_FIELDS = (
    "dominant_data_phy_rate_mbps",
    "dominant_data_profile_share",
    "effective_phy_rate_mbps",
    "data_tx_rate_over_interval_mbps",
    "data_tx_opportunity_gap_fraction",
    "data_tx_profile",
    "mean_dominant_data_phy_rate_mbps",
    "mean_effective_phy_rate_mbps",
    "aggregate_data_tx_rate_over_interval_mbps",
)
_PHY_FIELDS = _STATION_FIELDS + (
    "busy_time_us",
    "channel_utilization_percent",
    "uplink",
    "downlink",
)
_RESOURCE_USAGE_KEYS = (
    "schema_version",
    "experiment_id",
    "repetition_attempt",
    "sample_interval_ms",
    "peak_rss_bytes",
    "minimum_mem_available_bytes",
    "minimum_mem_available_percent",
    "wall_time_seconds",
    "exit_code",
    "monitor_mode",
)
_RESOURCE_SUMMARY_KEYS = (
    "schema_version",
    "complete_matrix",
    "requested_experiment_ids",
    "executed_experiment_ids",
    "auto_included_baseline_ids",
    "memory_reserve_percent",
    "calibrated_peak_rss_bytes",
    "worker_peak_estimate_bytes",
    "maximum_parallel_workers",
    "minimum_mem_available_bytes",
    "minimum_mem_available_percent",
    "attempts",
)
_TARGET_RSSI_DBM = {"high": -41.5, "medium": -50.0, "low": -60.0}
_FIXED_WIFI_METADATA = {
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
}
_FIXED_TCP_METADATA = {
    "congestion_control": "ns3::TcpHighSpeed",
    "segment_size_bytes": 1460,
    "send_buffer_bytes": 33554432,
    "receive_buffer_bytes": 33554432,
    "wired_rate": "10Gbps",
    "wired_delay": "0.1ms",
}
_TOLERANCE = 1e-9


class _AuditInputError(ValueError):
    """One path-bearing discrepancy discovered while reading retained data."""


@dataclass(frozen=True)
class _Profile:
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
class _Station:
    dominant: float | None
    share: float | None
    effective: float | None
    interval_rate: float
    gap: float | None
    profiles: tuple[_Profile, ...]


@dataclass(frozen=True)
class _Bss:
    mean_dominant: float | None
    mean_effective: float | None
    aggregate_interval_rate: float


@dataclass(frozen=True)
class _Attempt:
    configuration: ExperimentConfiguration
    repetition_attempt: int
    repetitions: int
    stations: dict[tuple[int, int], _Station]
    bsses: dict[int, _Bss]


@dataclass(frozen=True)
class AuditReport:
    """Counts and independently derived extrema for one retained run."""

    run_directory: Path
    experiment_attempt_count: int
    output_json_count: int
    csv_data_row_count: int
    csv_column_count: int
    resource_usage_count: int
    stdout_log_count: int
    stderr_log_count: int
    null_csv_cell_count: int
    signed_baseline_minimum: float | None
    signed_baseline_maximum: float | None
    minimum_mem_available_percent: float | None
    peak_rss_bytes: int | None
    maximum_parallel_workers: int | None
    discrepancies: tuple[str, ...]

    @property
    def ok(self) -> bool:
        """Return whether no discrepancy was found."""
        return not self.discrepancies

    def as_dict(self) -> dict[str, object]:
        """Return an ordered JSON-compatible report."""
        return {
            "run_directory": str(self.run_directory),
            "experiment_attempt_count": self.experiment_attempt_count,
            "output_json_count": self.output_json_count,
            "csv_data_row_count": self.csv_data_row_count,
            "csv_column_count": self.csv_column_count,
            "resource_usage_count": self.resource_usage_count,
            "stdout_log_count": self.stdout_log_count,
            "stderr_log_count": self.stderr_log_count,
            "null_csv_cell_count": self.null_csv_cell_count,
            "signed_baseline_minimum": self.signed_baseline_minimum,
            "signed_baseline_maximum": self.signed_baseline_maximum,
            "minimum_mem_available_percent": self.minimum_mem_available_percent,
            "peak_rss_bytes": self.peak_rss_bytes,
            "maximum_parallel_workers": self.maximum_parallel_workers,
            "discrepancy_count": len(self.discrepancies),
            "discrepancies": list(self.discrepancies),
        }


def _fail(path: Path, message: str) -> NoReturn:
    raise _AuditInputError(f"{path}: {message}")


def _load_json(path: Path) -> dict[str, object]:
    """Strictly parse one regular JSON file without validator reuse."""
    try:
        status = path.lstat()
    except OSError as error:
        _fail(path, f"cannot inspect JSON: {error}")
    if stat.S_ISLNK(status.st_mode) or not stat.S_ISREG(status.st_mode):
        _fail(path, "JSON must be a regular non-symlink file")

    def reject_duplicates(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                _fail(path, f"duplicate JSON object key {key!r}")
            result[key] = value
        return result

    def reject_constant(value: str) -> NoReturn:
        _fail(path, f"non-standard JSON number {value}")

    descriptor = -1
    try:
        descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC)
        if not stat.S_ISREG(os.fstat(descriptor).st_mode):
            _fail(path, "JSON descriptor is not a regular file")
        with os.fdopen(descriptor, "r", encoding="utf-8") as input_file:
            descriptor = -1
            value = json.load(
                input_file,
                object_pairs_hook=reject_duplicates,
                parse_constant=reject_constant,
            )
    except _AuditInputError:
        raise
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        _fail(path, f"cannot parse JSON: {error}")
    finally:
        if descriptor >= 0:
            os.close(descriptor)
    if type(value) is not dict:
        _fail(path, "JSON root must be an object")
    return value


def _number(value: object, path: Path, name: str, *, minimum: float | None = None) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        _fail(path, f"{name} must be a finite number")
    try:
        result = float(value)
    except (OverflowError, TypeError, ValueError):
        _fail(path, f"{name} must be a finite number")
    if not math.isfinite(result) or (minimum is not None and result < minimum):
        _fail(path, f"{name} must be finite and >= {minimum}")
    return result


def _integer(value: object, path: Path, name: str, *, minimum: int = 0) -> int:
    if type(value) is not int or value < minimum:
        _fail(path, f"{name} must be an integer >= {minimum}")
    return value


def _optional_number(
    value: object, path: Path, name: str, *, minimum: float | None = None
) -> float | None:
    if value is None:
        return None
    return _number(value, path, name, minimum=minimum)


def _near(left: float, right: float) -> bool:
    return abs(left - right) <= _TOLERANCE * max(1.0, abs(left), abs(right))


def _exact_scalar_object(value: object, expected: dict[str, object]) -> bool:
    return (
        type(value) is dict
        and tuple(value) == tuple(expected)
        and all(
            type(value[key]) is type(expected_value) and value[key] == expected_value
            for key, expected_value in expected.items()
        )
    )


def _same_optional(left: float | None, right: float | None) -> bool:
    return (left is None and right is None) or (
        left is not None and right is not None and _near(left, right)
    )


def _he_gi3200_rate_mbps(width: int, nss: int, mcs: int) -> float:
    """Reconstruct an HE SU GI-3200 data rate from first principles."""
    usable_subcarriers = {20: 234, 40: 468, 80: 980}[width]
    modulation_bits = (1, 2, 2, 4, 4, 6, 6, 6, 8, 8, 10, 10)[mcs]
    code_numerator = (1, 1, 3, 1, 3, 2, 3, 5, 3, 5, 3, 5)[mcs]
    code_denominator = (2, 2, 4, 2, 4, 3, 4, 6, 4, 6, 4, 6)[mcs]
    bits_per_symbol = (
        usable_subcarriers * modulation_bits * code_numerator / code_denominator
    )
    single_stream_bps = math.ceil(bits_per_symbol * 1_000_000_000 / 16_000)
    return single_stream_bps * nss / 1_000_000


def _parse_profile(value: object, path: Path, name: str) -> tuple[_Profile, ...]:
    if type(value) is not list:
        _fail(path, f"{name} profile must be an array")
    profiles = []
    previous: tuple[int, int, int] | None = None
    for index, raw in enumerate(value):
        item_name = f"{name}.data_tx_profile[{index}]"
        if type(raw) is not dict or tuple(raw) != _PROFILE_KEYS:
            _fail(path, f"{item_name} profile keys/order are invalid")
        width = _integer(raw["channel_width_mhz"], path, f"{item_name}.channel_width_mhz", minimum=1)
        nss = _integer(raw["nss"], path, f"{item_name}.nss", minimum=1)
        mcs = _integer(raw["mcs"], path, f"{item_name}.mcs")
        if width not in (20, 40, 80) or nss > 2 or mcs > 11:
            _fail(path, f"{item_name} is outside HE GI3200 width/NSS/MCS bounds")
        profile = _Profile(
            width,
            nss,
            mcs,
            _number(raw["transmitted_psdu_bytes"], path, f"{item_name}.transmitted_psdu_bytes", minimum=0.0),
            _integer(raw["ppdu_attempt_count"], path, f"{item_name}.ppdu_attempt_count"),
            _number(raw["ppdu_airtime_us"], path, f"{item_name}.ppdu_airtime_us", minimum=0.0),
        )
        if previous is not None and profile.key <= previous:
            _fail(path, f"{name} profile keys are duplicate or out of order")
        previous = profile.key
        profiles.append(profile)
    return tuple(profiles)


def _parse_station_phy(
    phy: object, interval_us: float, path: Path, name: str
) -> _Station:
    if type(phy) is not dict or tuple(phy) != _PHY_FIELDS:
        _fail(path, f"{name} station PHY field order is invalid")
    for field in _STATION_FIELDS:
        if field not in phy:
            _fail(path, f"{name}.{field} is missing")
    for field in _STATION_FIELDS[-3:]:
        if phy[field] is not None:
            _fail(path, f"{name}.{field} station contains BSS value")
    profiles = _parse_profile(phy["data_tx_profile"], path, name)
    dominant = _optional_number(phy["dominant_data_phy_rate_mbps"], path, f"{name}.dominant_data_phy_rate_mbps", minimum=0.0)
    share = _optional_number(phy["dominant_data_profile_share"], path, f"{name}.dominant_data_profile_share", minimum=0.0)
    effective = _optional_number(phy["effective_phy_rate_mbps"], path, f"{name}.effective_phy_rate_mbps", minimum=0.0)
    interval = _number(phy["data_tx_rate_over_interval_mbps"], path, f"{name}.data_tx_rate_over_interval_mbps", minimum=0.0)
    gap = _optional_number(phy["data_tx_opportunity_gap_fraction"], path, f"{name}.data_tx_opportunity_gap_fraction", minimum=0.0)
    if not profiles:
        if any(value is not None for value in (dominant, share, effective, gap)) or interval != 0.0:
            _fail(path, f"{name} idle station role is not null/null/null/zero/null")
        return _Station(None, None, None, 0.0, None, ())
    if any(value is None for value in (dominant, share, effective, gap)):
        _fail(path, f"{name} active station has null derived values")
    total_bytes = sum(item.transmitted_bytes for item in profiles)
    total_airtime = sum(item.airtime_us for item in profiles)
    if total_bytes <= 0.0 or total_airtime <= 0.0 or total_airtime > interval_us + _TOLERANCE:
        _fail(path, f"{name} profile has invalid sums")
    selected = max(
        profiles,
        key=lambda item: (
            item.transmitted_bytes,
            _he_gi3200_rate_mbps(*item.key),
            -item.width,
            -item.nss,
            -item.mcs,
        ),
    )
    wanted = {
        "dominant_data_phy_rate_mbps": _he_gi3200_rate_mbps(*selected.key),
        "dominant_data_profile_share": selected.transmitted_bytes / total_bytes,
        "effective_phy_rate_mbps": total_bytes * 8.0 / total_airtime,
        "data_tx_rate_over_interval_mbps": total_bytes * 8.0 / interval_us,
        "data_tx_opportunity_gap_fraction": 1.0 - total_airtime / interval_us,
    }
    observed = {
        "dominant_data_phy_rate_mbps": dominant,
        "dominant_data_profile_share": share,
        "effective_phy_rate_mbps": effective,
        "data_tx_rate_over_interval_mbps": interval,
        "data_tx_opportunity_gap_fraction": gap,
    }
    for field, expected in wanted.items():
        actual = observed[field]
        if actual is None or not _near(actual, expected):
            _fail(path, f"{name}.{field} does not reproduce profile formula")
    assert dominant is not None and share is not None and effective is not None and gap is not None
    return _Station(dominant, share, effective, interval, gap, profiles)


def _parse_bss_phy(phy: object, path: Path, name: str) -> _Bss:
    if type(phy) is not dict or tuple(phy) != _PHY_FIELDS:
        _fail(path, f"{name} BSS PHY field order is invalid")
    for field in _STATION_FIELDS[:5]:
        if phy[field] is not None:
            _fail(path, f"{name}.{field} BSS contains station value")
    if phy["data_tx_profile"] != []:
        _fail(path, f"{name} BSS profile must be empty")
    dominant = _optional_number(phy["mean_dominant_data_phy_rate_mbps"], path, f"{name}.mean_dominant_data_phy_rate_mbps", minimum=0.0)
    effective = _optional_number(phy["mean_effective_phy_rate_mbps"], path, f"{name}.mean_effective_phy_rate_mbps", minimum=0.0)
    aggregate = _number(phy["aggregate_data_tx_rate_over_interval_mbps"], path, f"{name}.aggregate_data_tx_rate_over_interval_mbps", minimum=0.0)
    return _Bss(dominant, effective, aggregate)


def _verify_bss(bss: _Bss, stations: list[_Station], path: Path, name: str) -> None:
    dominant_values = [item.dominant for item in stations if item.dominant is not None]
    effective_values = [item.effective for item in stations if item.effective is not None]
    wanted_dominant = sum(dominant_values) / len(dominant_values) if dominant_values else None
    wanted_effective = sum(effective_values) / len(effective_values) if effective_values else None
    wanted_aggregate = sum(item.interval_rate for item in stations)
    if not _same_optional(bss.mean_dominant, wanted_dominant):
        _fail(path, f"{name} BSS mean dominant formula mismatch")
    if not _same_optional(bss.mean_effective, wanted_effective):
        _fail(path, f"{name} BSS mean effective formula mismatch")
    if not _near(bss.aggregate_interval_rate, wanted_aggregate):
        _fail(path, f"{name} BSS aggregate formula mismatch")


def _identity_key(entity: object, path: Path, name: str, station: bool) -> tuple[int, int] | int:
    if type(entity) is not dict:
        _fail(path, f"{name} entity must be an object")
    bss_id = _integer(entity.get("access_point_id"), path, f"{name}.access_point_id")
    if station:
        return bss_id, _integer(entity.get("station_index"), path, f"{name}.station_index")
    return bss_id


def _parse_attempt(
    path: Path, configuration: ExperimentConfiguration, repetition_attempt: int
) -> _Attempt:
    root = _load_json(path)
    if tuple(root) != _ROOT_KEYS:
        _fail(path, "output root keys/order are invalid")
    if type(root["schema_version"]) is not int or root["schema_version"] != 2:
        _fail(path, "schema_version must be integer 2")
    semantics = root["measurement_semantics"]
    if (
        type(semantics) is not dict
        or tuple(semantics) != tuple(_MEASUREMENT_SEMANTICS)
        or semantics != _MEASUREMENT_SEMANTICS
    ):
        _fail(path, "measurement semantics keys/order/values are not exact")
    window_ms = _integer(root["statistics_window_ms"], path, "statistics_window_ms", minimum=1)
    if 1000 % window_ms:
        _fail(path, "statistics_window_ms must divide one second")
    validation = root["validation"]
    if (
        type(validation) is not dict
        or tuple(validation) != _VALIDATION_KEYS
        or any(validation[key] is not True for key in _VALIDATION_KEYS)
    ):
        _fail(path, "validation must contain the exact eight ordered true flags")
    metadata = root["experiment_metadata"]
    if type(metadata) is not dict:
        _fail(path, "experiment_metadata must be an object")
    actual = metadata.get("configuration")
    if type(actual) is not dict:
        _fail(path, "configuration metadata must be an object")
    if tuple(actual) != tuple(_CONFIGURATION_KEYS):
        _fail(path, "configuration sections/order are invalid")
    for section, fields in _CONFIGURATION_KEYS.items():
        if type(actual[section]) is not dict or tuple(actual[section]) != fields:
            _fail(path, f"configuration {section} fields/order are invalid")

    general = actual["general"]
    if (
        type(general["output_name"]) is not str
        or general["output_name"] != "output.json"
        or type(general["run_folder"]) is not str
        or not general["run_folder"]
    ):
        _fail(path, "general output_name/run_folder metadata is invalid")
    script = actual["script"]
    repetitions = _integer(
        script["repetitions"],
        path,
        "script.repetitions",
        minimum=1,
    )
    benchmark = actual["benchmark"]
    expected_benchmark = {
        "sta_count_per_bss": configuration.sta_count_per_bss,
        "rssi_range": configuration.rssi_range,
        "interference_mode": configuration.interference_mode,
        "traffic_mode": configuration.traffic_mode,
        "mimo_mode": configuration.mimo_mode,
    }
    if not _exact_scalar_object(benchmark, expected_benchmark):
        _fail(path, "configuration does not match matrix coordinate")
    simulation = actual["simulation"]
    if not _exact_scalar_object(
        simulation,
        {"rng_seed": 12345, "rng_run": repetition_attempt},
    ):
        _fail(path, "simulation seed/run metadata is invalid")
    wifi = actual["wifi"]
    if not _exact_scalar_object(wifi, _FIXED_WIFI_METADATA):
        _fail(path, "wifi fixed metadata is invalid")
    tcp = actual["tcp"]
    if not _exact_scalar_object(tcp, _FIXED_TCP_METADATA):
        _fail(path, "tcp fixed metadata is invalid")
    statistics = actual["statistics"]
    if not _exact_scalar_object(statistics, {"window_ms": window_ms}):
        _fail(path, "statistics window metadata mismatch")
    logging = actual["logging"]
    if not _exact_scalar_object(logging, {"scenario_level": "info"}):
        _fail(path, "logging fixed metadata is invalid")

    inventory = metadata.get("entity_inventory")
    if type(inventory) is not dict:
        _fail(path, "entity inventory is missing")
    ap_inventory = inventory.get("access_points")
    station_inventory = inventory.get("stations")
    if type(ap_inventory) is not list or type(station_inventory) is not list:
        _fail(path, "entity inventory arrays are missing")
    ap_keys = [_identity_key(item, path, "inventory AP", False) for item in ap_inventory]
    station_keys = [_identity_key(item, path, "inventory station", True) for item in station_inventory]
    expected_station_keys = [
        (bss_id, station_index)
        for bss_id in range(3)
        for station_index in range(configuration.sta_count_per_bss)
    ]
    if ap_keys != [0, 1, 2] or station_keys != expected_station_keys:
        _fail(path, "entity inventory order/count does not match three BSSs")

    windows = root["windows"]
    if type(windows) is not list:
        _fail(path, "windows must be an array")
    merged: dict[tuple[int, int], dict[tuple[int, int, int], list[float | int]]] = {}
    previous_window = -1
    for window_position, window in enumerate(windows):
        if type(window) is not dict:
            _fail(path, f"window {window_position} must be an object")
        window_index = _integer(window.get("window_index"), path, f"window {window_position} index")
        if window_index <= previous_window or window_index >= 1000 // window_ms:
            _fail(path, f"window {window_position} order/range is invalid")
        previous_window = window_index
        start = _number(window.get("window_start_ms"), path, f"window {window_position} start")
        duration = _number(window.get("window_duration_ms"), path, f"window {window_position} duration", minimum=0.0)
        if not _near(start, window_index * window_ms) or not _near(duration, window_ms):
            _fail(path, f"window {window_position} position is invalid")
        raw_stations = window.get("stations")
        raw_bsses = window.get("access_points")
        if type(raw_stations) is not list or type(raw_bsses) is not list:
            _fail(path, f"window {window_position} entity arrays are missing")
        if not raw_stations and not raw_bsses:
            _fail(path, f"window {window_position} is an empty sparse window")
        station_map: dict[tuple[int, int], _Station] = {}
        for raw_station in raw_stations:
            key = _identity_key(raw_station, path, f"window {window_position} station", True)
            assert isinstance(key, tuple)
            if key in station_map or key not in expected_station_keys:
                _fail(path, f"window {window_position} station order/identity is invalid")
            station = _parse_station_phy(raw_station.get("phy_stats"), duration * 1000.0, path, f"window {window_position} station {key}")
            if not station.profiles:
                _fail(path, f"window {window_position} contains inactive station")
            station_map[key] = station
            profile_map = merged.setdefault(key, {})
            for profile in station.profiles:
                total = profile_map.setdefault(profile.key, [0.0, 0, 0.0])
                total[0] += profile.transmitted_bytes
                total[1] += profile.attempts
                total[2] += profile.airtime_us
        if list(station_map) != sorted(station_map):
            _fail(path, f"window {window_position} station order is invalid")
        bss_map: dict[int, _Bss] = {}
        for raw_bss in raw_bsses:
            bss_id = _identity_key(raw_bss, path, f"window {window_position} BSS", False)
            assert isinstance(bss_id, int)
            if bss_id in bss_map:
                _fail(path, f"window {window_position} duplicate BSS")
            bss = _parse_bss_phy(raw_bss.get("phy_stats"), path, f"window {window_position} BSS {bss_id}")
            _verify_bss(
                bss,
                [item for key, item in station_map.items() if key[0] == bss_id],
                path,
                f"window {window_position} BSS {bss_id}",
            )
            bss_map[bss_id] = bss
        active_bsses = sorted({key[0] for key in station_map})
        if list(bss_map) != active_bsses:
            _fail(path, f"window {window_position} BSS parent order is invalid")

    overall = root["overall"]
    if type(overall) is not dict or tuple(overall) != ("access_points", "stations"):
        _fail(path, "overall shape/order is invalid")
    raw_overall_stations = overall["stations"]
    raw_overall_bsses = overall["access_points"]
    if type(raw_overall_stations) is not list or type(raw_overall_bsses) is not list:
        _fail(path, "overall arrays are missing")
    stations: dict[tuple[int, int], _Station] = {}
    for raw_station in raw_overall_stations:
        key = _identity_key(raw_station, path, "overall station", True)
        assert isinstance(key, tuple)
        if key in stations:
            _fail(path, "overall station is duplicated")
        stations[key] = _parse_station_phy(raw_station.get("phy_stats"), 1_000_000.0, path, f"overall station {key}")
    if list(stations) != expected_station_keys:
        _fail(path, "overall station inventory/order is not dense")
    for key, station in stations.items():
        merged_profiles = merged.get(key, {})
        if tuple(item.key for item in station.profiles) != tuple(sorted(merged_profiles)):
            _fail(path, f"overall station {key} profile keys do not reproduce windows")
        for profile in station.profiles:
            values = merged_profiles[profile.key]
            if (
                not _near(profile.transmitted_bytes, float(values[0]))
                or profile.attempts != values[1]
                or not _near(profile.airtime_us, float(values[2]))
            ):
                _fail(path, f"overall station {key} profile sums do not reproduce windows")
    bsses: dict[int, _Bss] = {}
    for raw_bss in raw_overall_bsses:
        bss_id = _identity_key(raw_bss, path, "overall BSS", False)
        assert isinstance(bss_id, int)
        if bss_id in bsses:
            _fail(path, "overall BSS is duplicated")
        bss = _parse_bss_phy(raw_bss.get("phy_stats"), path, f"overall BSS {bss_id}")
        _verify_bss(
            bss,
            [item for key, item in stations.items() if key[0] == bss_id],
            path,
            f"overall BSS {bss_id}",
        )
        bsses[bss_id] = bss
    if list(bsses) != [0, 1, 2]:
        _fail(path, "overall BSS inventory/order is not dense")
    return _Attempt(configuration, repetition_attempt, repetitions, stations, bsses)


def _compact(value: float) -> str:
    return format(value, ".15g")


def _render_profile_cell(profiles: tuple[_Profile, ...]) -> str:
    return "|".join(
        f"W{item.width}_NSS{item.nss}_MCS{item.mcs}:"
        f"bytes={_compact(item.transmitted_bytes)},ppdus={item.attempts},"
        f"airtime_us={_compact(item.airtime_us)}"
        for item in profiles
    )


def _header() -> tuple[str, ...]:
    columns = [
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
    for index in range(30):
        columns.extend(
            (
                f"sta_{index}_dominant_data_phy_rate_mbps",
                f"sta_{index}_dominant_data_profile_share",
                f"sta_{index}_effective_phy_rate_mbps",
                f"sta_{index}_data_tx_rate_over_interval_mbps",
                f"sta_{index}_data_tx_opportunity_gap_fraction",
                f"sta_{index}_tx_profile",
            )
        )
    return tuple(columns)


def _cell(value: object) -> str:
    return "" if value is None else str(value)


def _expected_rows(attempts: list[_Attempt]) -> tuple[list[list[str]], list[float]]:
    baselines: dict[tuple[str, str, str, str, int, int], float] = {}
    for attempt in attempts:
        configuration = attempt.configuration
        if configuration.sta_count_per_bss != 1:
            continue
        for bss_id, bss in attempt.bsses.items():
            baselines[(configuration.rssi_range, configuration.interference_mode, configuration.traffic_mode, configuration.mimo_mode, attempt.repetition_attempt, bss_id)] = bss.aggregate_interval_rate
    rows = []
    baseline_values = []
    for attempt in attempts:
        configuration = attempt.configuration
        for bss_id in range(3):
            bss = attempt.bsses[bss_id]
            key = (configuration.rssi_range, configuration.interference_mode, configuration.traffic_mode, configuration.mimo_mode, attempt.repetition_attempt, bss_id)
            if key not in baselines:
                raise _AuditInputError(
                    f"experiment {configuration.experiment_id}: matching baseline is missing"
                )
            baseline_rate = baselines[key]
            overhead = None if baseline_rate == 0.0 else 1.0 - bss.aggregate_interval_rate / baseline_rate
            if overhead is not None:
                baseline_values.append(overhead)
            row = [
                str(configuration.experiment_id),
                str(attempt.repetition_attempt),
                str(configuration.sta_count_per_bss),
                configuration.rssi_range,
                str(_TARGET_RSSI_DBM[configuration.rssi_range]),
                configuration.interference_mode,
                configuration.traffic_mode,
                configuration.mimo_mode,
                str(bss_id),
                _cell(bss.mean_dominant),
                _cell(bss.mean_effective),
                str(bss.aggregate_interval_rate),
                _cell(overhead),
            ]
            for station_index in range(30):
                if station_index >= configuration.sta_count_per_bss:
                    row.extend(("", "", "", "", "", ""))
                    continue
                station = attempt.stations[(bss_id, station_index)]
                row.extend(
                    (
                        _cell(station.dominant),
                        _cell(station.share),
                        _cell(station.effective),
                        str(station.interval_rate),
                        _cell(station.gap),
                        _render_profile_cell(station.profiles),
                    )
                )
            rows.append(row)
    return rows, baseline_values


def _read_csv(path: Path) -> tuple[list[list[str]], list[str]]:
    errors = []
    try:
        raw = path.read_bytes()
    except OSError as error:
        return [], [f"{path}: cannot read CSV: {error}"]
    if not raw.startswith(b"\xef\xbb\xbf"):
        errors.append(f"{path}: CSV is missing UTF-8 BOM")
        payload = raw
    else:
        payload = raw[3:]
    residual = payload.replace(b"\r\n", b"")
    if not payload.endswith(b"\r\n") or b"\r" in residual or b"\n" in residual:
        errors.append(f"{path}: CSV transport is not exact CRLF")
    try:
        text = payload.decode("utf-8")
        rows = list(csv.reader(StringIO(text, newline=""), delimiter=";"))
    except (UnicodeError, csv.Error) as error:
        errors.append(f"{path}: cannot parse CSV: {error}")
        rows = []
    if rows and tuple(rows[0]) != _header():
        errors.append(f"{path}: CSV header does not match the independent 193-column contract")
    return rows, errors


def _resource_selection(
    summary: dict[str, object], path: Path, discrepancies: list[str]
) -> tuple[list[int], list[dict[str, object]]]:
    if tuple(summary) != _RESOURCE_SUMMARY_KEYS:
        discrepancies.append(f"{path}: resource manifest summary keys/order are invalid")
    if type(summary.get("schema_version")) is not int or summary.get("schema_version") != 1:
        discrepancies.append(
            f"{path}: resource manifest schema_version must be integer 1"
        )
    requested = summary.get("requested_experiment_ids")
    executed = summary.get("executed_experiment_ids")
    automatic = summary.get("auto_included_baseline_ids")
    attempts = summary.get("attempts")
    if any(type(value) is not list for value in (requested, executed, automatic, attempts)):
        discrepancies.append(f"{path}: resource manifest arrays are invalid")
        return [], []
    assert isinstance(requested, list) and isinstance(executed, list)
    assert isinstance(automatic, list) and isinstance(attempts, list)
    matrix = build_matrix()
    by_id = {item.experiment_id: item for item in matrix}
    selection_valid = True
    for name, values, allow_empty in (
        ("requested_experiment_ids", requested, False),
        ("executed_experiment_ids", executed, False),
        ("auto_included_baseline_ids", automatic, True),
    ):
        if not allow_empty and not values:
            discrepancies.append(f"{path}: resource manifest {name} must be nonempty")
            selection_valid = False
        if any(type(value) is not int or value not in by_id for value in values):
            discrepancies.append(f"{path}: resource manifest {name} contains an invalid ID")
            selection_valid = False
        if len(values) != len(set(values)):
            discrepancies.append(f"{path}: resource manifest {name} must be unique")
            selection_valid = False

    expected_automatic = set()
    baseline_ids = {
        (item.rssi_range, item.interference_mode, item.traffic_mode, item.mimo_mode): item.experiment_id
        for item in matrix
        if item.sta_count_per_bss == 1
    }
    for experiment_id in requested:
        if type(experiment_id) is not int or experiment_id not in by_id:
            continue
        item = by_id[experiment_id]
        if item.sta_count_per_bss > 1:
            baseline_id = baseline_ids[(item.rssi_range, item.interference_mode, item.traffic_mode, item.mimo_mode)]
            if baseline_id not in requested:
                expected_automatic.add(baseline_id)
    if selection_valid and (
        executed != sorted(set(requested) | expected_automatic)
        or automatic != sorted(expected_automatic)
    ):
        discrepancies.append(f"{path}: resource manifest subset dependency expansion is invalid")
    complete = summary.get("complete_matrix")
    if type(complete) is not bool:
        discrepancies.append(f"{path}: resource manifest complete_matrix marker is invalid")
    elif complete and (
        requested != list(range(1, 127))
        or executed != list(range(1, 127))
        or automatic
    ):
        discrepancies.append(f"{path}: complete resource manifest selection is invalid")

    attempt_records = [item for item in attempts if type(item) is dict]
    if len(attempt_records) != len(attempts):
        discrepancies.append(f"{path}: resource manifest attempt entries must be objects")
    attempt_keys = []
    for record in attempt_records:
        experiment_id = record.get("experiment_id")
        repetition_attempt = record.get("repetition_attempt")
        if (
            type(experiment_id) is not int
            or experiment_id not in executed
            or type(repetition_attempt) is not int
            or repetition_attempt <= 0
        ):
            discrepancies.append(f"{path}: resource manifest attempt identity is invalid")
            continue
        attempt_keys.append((experiment_id, repetition_attempt))
    if len(attempt_keys) != len(set(attempt_keys)):
        discrepancies.append(f"{path}: resource manifest attempt keys must be unique")
    repetitions = sorted({key[1] for key in attempt_keys})
    expected_repetitions = list(range(1, max(repetitions, default=0) + 1))
    expected_lattice = [
        (experiment_id, repetition_attempt)
        for experiment_id in executed
        for repetition_attempt in expected_repetitions
    ]
    if not repetitions or repetitions != expected_repetitions or attempt_keys != expected_lattice:
        discrepancies.append(
            f"{path}: resource manifest attempts do not form the exact executed-ID/repetition lattice"
        )
    return executed, attempt_records


def _validate_resource_summary_aggregates(
    summary: dict[str, object],
    usage_records: list[dict[str, object]],
    path: Path,
    discrepancies: list[str],
) -> None:
    monitor_modes = {record["monitor_mode"] for record in usage_records}
    if len(monitor_modes) > 1:
        discrepancies.append(f"{path}: resource monitor modes must be consistent")
    if monitor_modes == {"sequential_fallback"}:
        for field in (
            "calibrated_peak_rss_bytes",
            "worker_peak_estimate_bytes",
            "minimum_mem_available_bytes",
            "minimum_mem_available_percent",
        ):
            if summary.get(field) is not None:
                discrepancies.append(
                    f"{path}: sequential_fallback requires null {field}"
                )
        if summary.get("maximum_parallel_workers") != 1:
            discrepancies.append(
                f"{path}: sequential_fallback requires exactly one worker"
            )

    reserve = summary.get("memory_reserve_percent")
    if type(reserve) is not int or not 15 <= reserve <= 50:
        discrepancies.append(
            f"{path}: memory_reserve_percent must be an integer in [15, 50]"
        )

    by_key = {
        (record["experiment_id"], record["repetition_attempt"]): record
        for record in usage_records
    }
    if summary.get("complete_matrix") is True:
        calibration_key = (126, 1)
    else:
        calibration_key = min(by_key, default=None)
    expected_calibrated_peak = (
        by_key[calibration_key]["peak_rss_bytes"]
        if calibration_key in by_key
        else None
    )
    calibrated_peak = summary.get("calibrated_peak_rss_bytes")
    if calibrated_peak != expected_calibrated_peak or (
        calibrated_peak is not None
        and (type(calibrated_peak) is not int or calibrated_peak < 0)
    ):
        discrepancies.append(
            f"{path}: calibrated_peak_rss_bytes does not match the calibration attempt"
        )

    peaks = [
        record["peak_rss_bytes"]
        for record in usage_records
        if record.get("peak_rss_bytes") is not None
    ]
    worker_estimate = summary.get("worker_peak_estimate_bytes")
    if peaks:
        required_estimate = (max(peaks) * 5 + 3) // 4
        if (
            type(worker_estimate) is not int
            or worker_estimate != required_estimate
        ):
            discrepancies.append(
                f"{path}: worker_peak_estimate_bytes does not equal the observed 1.25 margin"
            )
    elif worker_estimate is not None:
        discrepancies.append(
            f"{path}: worker_peak_estimate_bytes must be null without RSS measurements"
        )

    minimum_bytes_values = [
        record["minimum_mem_available_bytes"]
        for record in usage_records
        if record.get("minimum_mem_available_bytes") is not None
    ]
    summary_minimum_bytes = summary.get("minimum_mem_available_bytes")
    if minimum_bytes_values:
        if (
            type(summary_minimum_bytes) is not int
            or summary_minimum_bytes < 0
            or summary_minimum_bytes > min(minimum_bytes_values)
        ):
            discrepancies.append(
                f"{path}: minimum_mem_available_bytes is not a valid run minimum"
            )
    elif summary_minimum_bytes is not None:
        discrepancies.append(
            f"{path}: minimum_mem_available_bytes must be null without memory measurements"
        )

    minimum_percent_values = [
        float(record["minimum_mem_available_percent"])
        for record in usage_records
        if record.get("minimum_mem_available_percent") is not None
    ]
    summary_minimum_percent = summary.get("minimum_mem_available_percent")
    if minimum_percent_values:
        if (
            isinstance(summary_minimum_percent, bool)
            or not isinstance(summary_minimum_percent, (int, float))
            or not math.isfinite(float(summary_minimum_percent))
            or not 0.0 <= float(summary_minimum_percent) <= 100.0
            or float(summary_minimum_percent) > min(minimum_percent_values) + _TOLERANCE
        ):
            discrepancies.append(
                f"{path}: minimum_mem_available_percent is not a valid run minimum"
            )
    elif summary_minimum_percent is not None:
        discrepancies.append(
            f"{path}: minimum_mem_available_percent must be null without memory measurements"
        )


def _validate_declared_repetition_lattice(
    attempts: list[_Attempt],
    executed_ids: list[int],
    path: Path,
    discrepancies: list[str],
) -> None:
    declared_repetitions = {attempt.repetitions for attempt in attempts}
    if len(declared_repetitions) != 1:
        discrepancies.append(
            f"{path}: outputs do not declare one consistent script.repetitions value"
        )
        return
    repetitions = next(iter(declared_repetitions))
    expected = [
        (experiment_id, repetition_attempt)
        for experiment_id in executed_ids
        for repetition_attempt in range(1, repetitions + 1)
    ]
    observed = [
        (attempt.configuration.experiment_id, attempt.repetition_attempt)
        for attempt in attempts
    ]
    if observed != expected:
        discrepancies.append(
            f"{path}: attempts do not match the output-declared repetition lattice"
        )


def audit_run_directory(run_directory: str | Path) -> AuditReport:
    """Audit a retained run without modifying it and return every discrepancy."""
    run = Path(run_directory).resolve()
    discrepancies: list[str] = []
    summary_path = run / "resource_summary.json"
    try:
        summary = _load_json(summary_path)
    except _AuditInputError as error:
        discrepancies.append(str(error))
        summary = {}
    executed_ids, summary_attempt_records = _resource_selection(summary, summary_path, discrepancies)

    attempt_directories = sorted(run.glob("experiment_*/attempt_*")) if run.is_dir() else []
    output_paths = [path / "output.json" for path in attempt_directories if (path / "output.json").is_file()]
    usage_paths = [path / "resource_usage.json" for path in attempt_directories if (path / "resource_usage.json").is_file()]
    stdout_paths = [path / "stdout.log" for path in attempt_directories if (path / "stdout.log").is_file()]
    stderr_paths = [path / "stderr.log" for path in attempt_directories if (path / "stderr.log").is_file()]
    expected_attempt_count = len(summary_attempt_records)
    for count, name in (
        (len(attempt_directories), "attempt directory"),
        (len(output_paths), "output JSON"),
        (len(usage_paths), "resource usage"),
        (len(stdout_paths), "stdout log"),
        (len(stderr_paths), "stderr log"),
    ):
        if count != expected_attempt_count:
            discrepancies.append(
                f"{run}: {name} count {count} does not match resource manifest attempt count {expected_attempt_count}"
            )

    matrix = {item.experiment_id: item for item in build_matrix()}
    parsed_attempts: list[_Attempt] = []
    usage_records = []
    for attempt_directory in attempt_directories:
        try:
            experiment_id = int(attempt_directory.parent.name.removeprefix("experiment_"))
            repetition = int(attempt_directory.name.removeprefix("attempt_"))
        except ValueError:
            discrepancies.append(f"{attempt_directory}: attempt path components are invalid")
            continue
        if experiment_id not in matrix or repetition <= 0:
            discrepancies.append(f"{attempt_directory}: matrix ID or repetition is invalid")
            continue
        if experiment_id not in executed_ids:
            discrepancies.append(f"{attempt_directory}: attempt is absent from resource manifest executed IDs")
        output_path = attempt_directory / "output.json"
        if output_path.is_file():
            try:
                parsed_attempts.append(_parse_attempt(output_path, matrix[experiment_id], repetition))
            except _AuditInputError as error:
                discrepancies.append(str(error))
        usage_path = attempt_directory / "resource_usage.json"
        if usage_path.is_file():
            try:
                usage = _load_json(usage_path)
                if tuple(usage) != _RESOURCE_USAGE_KEYS:
                    _fail(usage_path, "resource usage schema keys/order are invalid")
                if type(usage.get("schema_version")) is not int or usage.get("schema_version") != 1:
                    _fail(
                        usage_path,
                        "resource usage schema_version must be integer 1",
                    )
                if (
                    _integer(usage.get("experiment_id"), usage_path, "experiment_id")
                    != experiment_id
                    or _integer(
                        usage.get("repetition_attempt"),
                        usage_path,
                        "repetition_attempt",
                        minimum=1,
                    )
                    != repetition
                ):
                    _fail(usage_path, "resource usage identity is invalid")
                sample_interval_ms = usage.get("sample_interval_ms")
                if type(sample_interval_ms) is not int or sample_interval_ms != 100:
                    _fail(usage_path, "sample_interval_ms must be integer 100")
                monitor_mode = usage.get("monitor_mode")
                if monitor_mode not in ("linux_proc", "sequential_fallback"):
                    _fail(usage_path, "resource monitor_mode is invalid")
                resource_fields = (
                    "peak_rss_bytes",
                    "minimum_mem_available_bytes",
                    "minimum_mem_available_percent",
                )
                if monitor_mode == "linux_proc":
                    _integer(
                        usage.get("peak_rss_bytes"),
                        usage_path,
                        "linux_proc requires numeric peak_rss_bytes",
                    )
                    _integer(
                        usage.get("minimum_mem_available_bytes"),
                        usage_path,
                        "linux_proc requires numeric minimum_mem_available_bytes",
                    )
                    value = _number(
                        usage.get("minimum_mem_available_percent"),
                        usage_path,
                        "linux_proc requires numeric minimum_mem_available_percent",
                        minimum=0.0,
                    )
                    if value > 100.0:
                        _fail(usage_path, "minimum_mem_available_percent exceeds 100")
                else:
                    for field in resource_fields:
                        if usage.get(field) is not None:
                            _fail(
                                usage_path,
                                f"sequential_fallback requires null {field}",
                            )
                _number(usage.get("wall_time_seconds"), usage_path, "wall_time_seconds", minimum=0.0)
                exit_code = _integer(usage.get("exit_code"), usage_path, "exit_code")
                if exit_code != 0:
                    _fail(usage_path, "exit_code must be zero for an accepted attempt")
                usage_records.append(usage)
            except _AuditInputError as error:
                discrepancies.append(str(error))

    expected_resource_order = sorted(
        usage_records, key=lambda item: (item["experiment_id"], item["repetition_attempt"])
    )
    if summary_attempt_records != expected_resource_order:
        discrepancies.append(f"{summary_path}: resource manifest attempts do not exactly reproduce usage files")
    _validate_resource_summary_aggregates(
        summary,
        usage_records,
        summary_path,
        discrepancies,
    )
    parsed_attempts.sort(
        key=lambda item: (
            item.configuration.experiment_id,
            item.repetition_attempt,
        )
    )
    _validate_declared_repetition_lattice(
        parsed_attempts,
        executed_ids,
        summary_path,
        discrepancies,
    )
    baseline_values: list[float] = []
    expected_rows: list[list[str]] = []
    try:
        expected_rows, baseline_values = _expected_rows(parsed_attempts)
    except _AuditInputError as error:
        discrepancies.append(str(error))

    csv_path = run / "results.csv"
    csv_rows, csv_errors = _read_csv(csv_path)
    discrepancies.extend(csv_errors)
    data_rows = csv_rows[1:] if csv_rows else []
    if any(len(row) != 193 for row in csv_rows):
        discrepancies.append(f"{csv_path}: CSV contains a row with a non-193 column count")
    if data_rows != expected_rows:
        reason = "row order" if sorted(data_rows) == sorted(expected_rows) else "cell/baseline value"
        discrepancies.append(f"{csv_path}: CSV {reason} differs from independently reconstructed rows")

    minimum_percent_values = [
        float(item["minimum_mem_available_percent"])
        for item in usage_records
        if item.get("minimum_mem_available_percent") is not None
    ]
    usage_peaks = [
        int(item["peak_rss_bytes"])
        for item in usage_records
        if item.get("peak_rss_bytes") is not None
    ]
    derived_minimum = min(minimum_percent_values, default=None)
    summary_minimum = summary.get("minimum_mem_available_percent")
    if summary_minimum is not None:
        try:
            summary_minimum_value = _number(summary_minimum, summary_path, "minimum_mem_available_percent", minimum=0.0)
        except _AuditInputError as error:
            discrepancies.append(str(error))
        else:
            if derived_minimum is not None and summary_minimum_value > derived_minimum + _TOLERANCE:
                discrepancies.append(f"{summary_path}: resource summary minimum exceeds attempt minimum")
            derived_minimum = min(
                value for value in (derived_minimum, summary_minimum_value) if value is not None
            )
    maximum_workers = summary.get("maximum_parallel_workers")
    if (
        type(maximum_workers) is not int
        or maximum_workers < 0
        or (expected_attempt_count == 0 and maximum_workers != 0)
        or (expected_attempt_count > 0 and maximum_workers < 1)
    ):
        discrepancies.append(f"{summary_path}: maximum_parallel_workers is invalid")
        maximum_workers = None
    null_cells = sum(cell == "" for row in data_rows for cell in row)
    return AuditReport(
        run_directory=run,
        experiment_attempt_count=len(attempt_directories),
        output_json_count=len(output_paths),
        csv_data_row_count=len(data_rows),
        csv_column_count=len(csv_rows[0]) if csv_rows else 0,
        resource_usage_count=len(usage_paths),
        stdout_log_count=len(stdout_paths),
        stderr_log_count=len(stderr_paths),
        null_csv_cell_count=null_cells,
        signed_baseline_minimum=min(baseline_values, default=None),
        signed_baseline_maximum=max(baseline_values, default=None),
        minimum_mem_available_percent=derived_minimum,
        peak_rss_bytes=max(usage_peaks, default=None),
        maximum_parallel_workers=maximum_workers,
        discrepancies=tuple(discrepancies),
    )
