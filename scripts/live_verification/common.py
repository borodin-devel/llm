"""Shared policy, schema constants, validation primitives, and command builders."""

from __future__ import annotations

import math
from pathlib import Path
import shlex


POLICY = {
    "1W_high_load_1s.json": {"mode": "auto", "timeout_seconds": 900},
    "1W_high_load_10s.json": {"mode": "auto", "timeout_seconds": 3600},
    "1W_high_load_1m.json": {
        "mode": "fixed", "seconds": 1.0, "timeout_seconds": 1800
    },
    "1W_high_load_10m.json": {
        "mode": "fixed", "seconds": 1.0, "timeout_seconds": 1800
    },
}

ROOT_KEYS = (
    "schema_version", "measurement_semantics", "statistics_window_ms", "windows",
    "overall", "validation", "experiment_metadata",
)
MEASUREMENT_SEMANTICS = {
    "access_point_role": "BSS parent aggregate",
    "station_role": "per-station child detail",
    "parent_child_duplication": "intentional",
    "mac_tcp_payload_bytes": "header-based estimates",
    "phy_tagged_payload_bytes": "attempts and retransmissions included",
    "phy_unique_tagged_payload_bytes": "first tagged MPDU transmissions only",
    "phy_average_data_rate": "airtime-weighted",
    "congestion_window": "time-weighted per connection",
    "sample_distributions": "sample-weighted",
    "sparse_window_absence": "zero activity",
    "undefined_derived_values": None,
}
WINDOW_KEYS = {
    "window_index", "window_start_ms", "window_duration_ms", "access_points", "stations",
}
AP_IDENTITY_KEYS = {"access_point_id", "node_id", "node_label", "ipv4"}
STA_IDENTITY_KEYS = AP_IDENTITY_KEYS | {"station_index"}
CATEGORY_KEYS = {"general_stats", "app_stats", "tcp_stats", "mac_stats", "phy_stats"}
DIRECTIONS = {"uplink", "downlink"}
GENERAL_KEYS = {
    "estimated_transmitted_tcp_payload_bytes", "estimated_matched_tcp_payload_bytes",
    "matched_packet_count", "total_transmission_duration_us",
    "average_transmission_duration_us", "transmission_duration_standard_deviation_us",
    "minimum_transmission_duration_us", "maximum_transmission_duration_us",
    "effective_throughput_mbps", "application_to_phy_delay",
}
SAMPLE_KEYS = {
    "sample_count", "average_us", "standard_deviation_us", "minimum_us", "maximum_us",
}
APP_KEYS = {
    "accepted_send_count", "accepted_payload_bytes", "accepted_throughput_mbps",
    "receive_event_count", "received_payload_bytes", "received_throughput_mbps",
    "drop_event_count", "dropped_payload_bytes", "receive_interarrival_time", "agents", "peers",
}
APP_AGENT_KEYS = {
    "agent_key", "accepted_send_count", "accepted_payload_bytes", "accepted_throughput_mbps",
    "accepted_bandwidth_share_percent", "drop_event_count", "dropped_payload_bytes",
}
APP_PEER_KEYS = {
    "peer_node_id", "peer_ipv4", "accepted_send_count", "accepted_payload_bytes",
    "accepted_throughput_mbps", "accepted_bandwidth_share_percent", "receive_event_count",
    "received_payload_bytes", "received_throughput_mbps", "received_bandwidth_share_percent",
    "drop_event_count", "dropped_payload_bytes",
}
TCP_KEYS = {"connections"}
TCP_CONNECTION_KEYS = {
    "peer_node_id", "peer_ipv4", "congestion_window_observation_duration_us",
    "average_congestion_window_bytes", "last_congestion_window_bytes", "round_trip_time",
}
MAC_KEYS = {
    "estimated_transmit_event_count", "estimated_transmitted_tcp_payload_bytes",
    "estimated_transmit_throughput_mbps", "estimated_receive_event_count",
    "estimated_received_tcp_payload_bytes", "estimated_receive_throughput_mbps",
    "transmit_drop_count", "transmit_drop_packet_bytes", "mpdu_drop_count", "mpdu_drop_bytes",
    "data_failure_count", "final_data_failure_count", "mpdu_drops_by_reason", "peers",
}
MAC_PEER_KEYS = {
    "peer_node_id", "peer_ipv4", "estimated_transmit_event_count",
    "estimated_transmitted_tcp_payload_bytes", "estimated_transmit_throughput_mbps",
    "estimated_receive_event_count", "estimated_received_tcp_payload_bytes",
    "estimated_receive_throughput_mbps", "mpdu_drop_count", "mpdu_drop_bytes",
    "data_failure_count", "final_data_failure_count", "mpdu_drops_by_reason",
}
MAC_REASON_KEYS = {"reason_code", "drop_count"}
PHY_KEYS = (
    "dominant_data_phy_rate_mbps", "dominant_data_profile_share",
    "effective_phy_rate_mbps", "data_tx_rate_over_interval_mbps",
    "data_tx_opportunity_gap_fraction", "data_tx_profile",
    "mean_dominant_data_phy_rate_mbps", "mean_effective_phy_rate_mbps",
    "aggregate_data_tx_rate_over_interval_mbps", "busy_time_us",
    "channel_utilization_percent", "uplink", "downlink",
)
DATA_TX_PROFILE_KEYS = (
    "channel_width_mhz", "nss", "mcs", "transmitted_psdu_bytes", "ppdu_attempt_count",
    "ppdu_airtime_us",
)
PHY_DIRECTION_KEYS = {
    "tagged_payload_bytes", "unique_tagged_payload_bytes", "tagged_mpdu_count",
    "complete_tagged_mpdu_bytes", "transmission_attempt_count", "retransmission_count",
    "transmission_airtime_us", "average_data_rate_mbps", "throughput_mbps", "peers",
}
PHY_PEER_KEYS = {
    "peer_node_id", "peer_ipv4", "tagged_payload_bytes", "unique_tagged_payload_bytes",
    "transmission_attempt_count", "retransmission_count", "transmission_airtime_us",
    "average_data_rate_mbps", "throughput_mbps",
}
VALIDATION_KEYS = {
    "entity_inventory_references_valid", "app_agent_totals_consistent",
    "app_peer_totals_consistent", "mac_peer_totals_consistent",
    "phy_peer_totals_consistent", "ap_station_sender_totals_consistent",
    "overall_matches_windows", "unique_phy_payload_within_tagged_payload",
}
CONFIGURATION_KEYS = {
    "general": {"trace_file", "run_folder", "output_name"},
    "simulation": {
        "duration_mode", "fixed_duration_seconds", "auto_tail_seconds", "rng_seed", "rng_run"
    },
    "topology": {
        "bss_count", "stations_per_bss", "bss_spacing_m", "station_radius_m",
        "isolate_bss_channels", "ssid_prefix", "ap_sink_port", "station_sink_base_port",
        "generator_start_seconds",
    },
    "distribution": {"max_agents_per_station", "low_contention_priority", "slot_ms"},
    "wifi": {
        "band", "channel_number", "bandwidth_mhz", "primary_20_index", "rate_manager",
        "active_probing",
    },
    "tcp": {
        "congestion_control", "segment_size_bytes", "send_buffer_bytes", "receive_buffer_bytes"
    },
    "statistics": {"window_ms"},
    "logging": {
        "sample_scenario_level", "ap_generator_level", "sta_generator_level",
        "traffic_sink_level", "contention_distribution_level",
    },
}
REMOVED_KEYS = {
    "wifi_windows", "wifi_summary", "transmission_summary", "cross_layer_summary",
    "one_second_intervals", "average_theoretical_phy_rate_mbps",
    "average_practical_phy_rate_mbps", "channel_efficiency", "contention_fraction",
}
LEGACY_REPORT_MARKERS = (
    "APGenerator per-second statistics", "StaLlmGenerator per-second statistics",
    "[Final per-second]", "[Final overall]", "[Received Stats]",
)


class LiveTraceError(RuntimeError):
    """A path-bearing live verification failure."""


def validate_policy_coverage(discovered, trace_directory, policy=POLICY):
    """Return exact discovered trace names or reject policy drift."""
    discovered_names = {Path(path).name for path in discovered}
    policy_names = set(policy)
    missing = sorted(policy_names - discovered_names)
    unknown = sorted(discovered_names - policy_names)
    if missing or unknown:
        details = []
        if missing:
            details.append("missing discovered trace(s): " + ", ".join(missing))
        if unknown:
            details.append("unknown discovered trace(s): " + ", ".join(unknown))
        raise LiveTraceError(
            f"{trace_directory}: policy/discovery mismatch: {'; '.join(details)}"
        )
    return tuple(policy)


def build_llm_command(trace_path, run_directory, policy):
    """Build the one ns-3 command for a policy entry."""
    arguments = [
        "llm-scenario", "--config", "contrib/llm/config/llm_config.toml",
        "--general-trace-file", str(trace_path), "--general-run-folder", str(run_directory),
    ]
    if policy.get("mode") == "fixed":
        arguments.extend([
            "--simulation-duration-mode", "fixed",
            "--simulation-fixed-duration-seconds", str(policy.get("seconds")),
        ])
    elif policy.get("mode") != "auto":
        raise LiveTraceError(f"{trace_path}: unknown live policy mode: {policy.get('mode')!r}")
    return ["./ns3", "run", shlex.join(arguments)]


def fail(source_path, json_path, message):
    """Raise a path-bearing schema failure."""
    raise LiveTraceError(f"{source_path}: {json_path}: {message}")


def expect_object_keys(value, expected, source_path, json_path):
    if not isinstance(value, dict):
        fail(source_path, json_path, "expected object")
    actual = set(value)
    if actual != set(expected):
        missing = sorted(set(expected) - actual)
        extra = sorted(actual - set(expected))
        details = []
        if missing:
            details.append("missing " + ", ".join(missing))
        if extra:
            details.append("unexpected " + ", ".join(extra))
        fail(source_path, json_path, "wrong fields: " + "; ".join(details))


def expect_ordered_object_keys(value, expected, source_path, json_path):
    """Require an object with exactly the expected insertion-ordered fields."""
    expect_object_keys(value, expected, source_path, json_path)
    if tuple(value) != tuple(expected):
        fail(source_path, json_path, "fields are not in the required order")


def expect_list(value, source_path, json_path):
    if not isinstance(value, list):
        fail(source_path, json_path, "expected array")


def expect_nonnegative_integer(value, source_path, json_path, *, positive=False, maximum=None):
    if type(value) is not int or value < (1 if positive else 0):
        qualifier = "positive" if positive else "non-negative"
        fail(source_path, json_path, f"expected {qualifier} integer")
    if maximum is not None and value > maximum:
        fail(source_path, json_path, f"expected integer no greater than {maximum}")


def expect_finite_number(value, source_path, json_path, *, positive=False, maximum=None):
    finite = type(value) is int or (type(value) is float and math.isfinite(value))
    if not finite or value < 0:
        qualifier = "positive " if positive else "non-negative "
        fail(source_path, json_path, f"expected finite {qualifier}number")
    if positive and value <= 0:
        fail(source_path, json_path, "expected finite positive number")
    if maximum is not None and value > maximum:
        fail(source_path, json_path, f"expected number no greater than {maximum}")


def expect_optional_finite_number(value, source_path, json_path, *, maximum=None):
    if value is not None:
        expect_finite_number(value, source_path, json_path, maximum=maximum)


def expect_optional_nonnegative_integer(value, source_path, json_path):
    if value is not None:
        expect_nonnegative_integer(value, source_path, json_path)


def expect_string(value, source_path, json_path, *, allow_none=False, allowed=None):
    if allow_none and value is None:
        return
    if type(value) is not str or not value:
        fail(source_path, json_path, "expected nonempty string")
    if allowed is not None and value not in allowed:
        fail(source_path, json_path, "expected one of " + ", ".join(sorted(allowed)))


def expect_boolean(value, source_path, json_path):
    if type(value) is not bool:
        fail(source_path, json_path, "expected Boolean")


def validate_integer_fields(value, fields, source_path, json_path):
    for field in fields:
        expect_nonnegative_integer(value[field], source_path, f"{json_path}.{field}")


def validate_optional_number_fields(value, fields, source_path, json_path, *, maximum=None):
    for field in fields:
        expect_optional_finite_number(
            value[field], source_path, f"{json_path}.{field}", maximum=maximum
        )


def is_nonnegative_integer(value):
    return type(value) is int and value >= 0


def reject_removed_keys(value, source_path, json_path):
    if isinstance(value, dict):
        for key, child in value.items():
            child_path = f"{json_path}.{key}"
            if key in REMOVED_KEYS:
                fail(source_path, child_path, f"removed field {key!r} remains")
            reject_removed_keys(child, source_path, child_path)
    elif isinstance(value, list):
        for index, child in enumerate(value):
            reject_removed_keys(child, source_path, f"{json_path}[{index}]")


def validate_ordered_unique(records, key_function, source_path, json_path, label):
    """Reject duplicate or non-increasing deterministic array keys."""
    seen = set()
    previous = None
    for index, record in enumerate(records):
        key = key_function(record)
        record_path = f"{json_path}[{index}]"
        if key in seen:
            fail(source_path, record_path, f"duplicate {label} {key!r}")
        if previous is not None and key <= previous:
            fail(source_path, record_path, f"expected increasing {label} order")
        seen.add(key)
        previous = key


def console_text(value):
    if value is None:
        return ""
    if isinstance(value, bytes):
        return value.decode("utf-8", errors="replace")
    return str(value)


def format_run_failure(trace_path, command, return_code, console, reason):
    lines = console.splitlines()
    tail = "\n".join(lines[-200:])
    return (
        f"{trace_path}: {reason}\ntrace: {trace_path}\n"
        f"command: {shlex.join(str(part) for part in command)}\n"
        f"return code: {return_code}\nlast 200 console lines:\n{tail}"
    )


def reject_legacy_console(console, source_path):
    """Reject removed final measurement banners and rows."""
    for marker in LEGACY_REPORT_MARKERS:
        if marker in console:
            raise LiveTraceError(
                f"{source_path}: console contains legacy report marker {marker!r}"
            )
