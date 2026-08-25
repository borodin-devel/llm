#!/usr/bin/env python3
"""Run deterministic shape checks or the complete live trace matrix."""

from __future__ import annotations

import argparse
import copy
import json
import math
import os
from pathlib import Path
import secrets
import shlex
import stat
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock


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

ROOT_KEYS = {
    "schema_version",
    "measurement_semantics",
    "statistics_window_ms",
    "windows",
    "overall",
    "validation",
    "experiment_metadata",
}
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
    "window_index",
    "window_start_ms",
    "window_duration_ms",
    "access_points",
    "stations",
}
AP_IDENTITY_KEYS = {"access_point_id", "node_id", "node_label", "ipv4"}
STA_IDENTITY_KEYS = AP_IDENTITY_KEYS | {"station_index"}
CATEGORY_KEYS = {"general_stats", "app_stats", "tcp_stats", "mac_stats", "phy_stats"}
DIRECTIONS = {"uplink", "downlink"}
GENERAL_KEYS = {
    "estimated_transmitted_tcp_payload_bytes",
    "estimated_matched_tcp_payload_bytes",
    "matched_packet_count",
    "total_transmission_duration_us",
    "average_transmission_duration_us",
    "transmission_duration_standard_deviation_us",
    "minimum_transmission_duration_us",
    "maximum_transmission_duration_us",
    "effective_throughput_mbps",
    "application_to_phy_delay",
}
SAMPLE_KEYS = {
    "sample_count",
    "average_us",
    "standard_deviation_us",
    "minimum_us",
    "maximum_us",
}
APP_KEYS = {
    "accepted_send_count",
    "accepted_payload_bytes",
    "accepted_throughput_mbps",
    "receive_event_count",
    "received_payload_bytes",
    "received_throughput_mbps",
    "drop_event_count",
    "dropped_payload_bytes",
    "receive_interarrival_time",
    "agents",
    "peers",
}
APP_AGENT_KEYS = {
    "agent_key",
    "accepted_send_count",
    "accepted_payload_bytes",
    "accepted_throughput_mbps",
    "accepted_bandwidth_share_percent",
    "drop_event_count",
    "dropped_payload_bytes",
}
APP_PEER_KEYS = {
    "peer_node_id",
    "peer_ipv4",
    "accepted_send_count",
    "accepted_payload_bytes",
    "accepted_throughput_mbps",
    "accepted_bandwidth_share_percent",
    "receive_event_count",
    "received_payload_bytes",
    "received_throughput_mbps",
    "received_bandwidth_share_percent",
    "drop_event_count",
    "dropped_payload_bytes",
}
TCP_KEYS = {"connections"}
TCP_CONNECTION_KEYS = {
    "peer_node_id",
    "peer_ipv4",
    "congestion_window_observation_duration_us",
    "average_congestion_window_bytes",
    "last_congestion_window_bytes",
    "round_trip_time",
}
MAC_KEYS = {
    "estimated_transmit_event_count",
    "estimated_transmitted_tcp_payload_bytes",
    "estimated_transmit_throughput_mbps",
    "estimated_receive_event_count",
    "estimated_received_tcp_payload_bytes",
    "estimated_receive_throughput_mbps",
    "transmit_drop_count",
    "transmit_drop_packet_bytes",
    "mpdu_drop_count",
    "mpdu_drop_bytes",
    "data_failure_count",
    "final_data_failure_count",
    "mpdu_drops_by_reason",
    "peers",
}
MAC_PEER_KEYS = {
    "peer_node_id",
    "peer_ipv4",
    "estimated_transmit_event_count",
    "estimated_transmitted_tcp_payload_bytes",
    "estimated_transmit_throughput_mbps",
    "estimated_receive_event_count",
    "estimated_received_tcp_payload_bytes",
    "estimated_receive_throughput_mbps",
    "mpdu_drop_count",
    "mpdu_drop_bytes",
    "data_failure_count",
    "final_data_failure_count",
    "mpdu_drops_by_reason",
}
MAC_REASON_KEYS = {"reason_code", "drop_count"}
PHY_KEYS = {"busy_time_us", "channel_utilization_percent", "uplink", "downlink"}
PHY_DIRECTION_KEYS = {
    "tagged_payload_bytes",
    "unique_tagged_payload_bytes",
    "tagged_mpdu_count",
    "complete_tagged_mpdu_bytes",
    "transmission_attempt_count",
    "retransmission_count",
    "transmission_airtime_us",
    "average_data_rate_mbps",
    "throughput_mbps",
    "peers",
}
PHY_PEER_KEYS = {
    "peer_node_id",
    "peer_ipv4",
    "tagged_payload_bytes",
    "unique_tagged_payload_bytes",
    "transmission_attempt_count",
    "retransmission_count",
    "transmission_airtime_us",
    "average_data_rate_mbps",
    "throughput_mbps",
}
VALIDATION_KEYS = {
    "entity_inventory_references_valid",
    "app_agent_totals_consistent",
    "app_peer_totals_consistent",
    "mac_peer_totals_consistent",
    "phy_peer_totals_consistent",
    "ap_station_sender_totals_consistent",
    "overall_matches_windows",
    "unique_phy_payload_within_tagged_payload",
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
    "wifi_windows",
    "wifi_summary",
    "transmission_summary",
    "cross_layer_summary",
    "one_second_intervals",
}
LEGACY_REPORT_MARKERS = (
    "APGenerator per-second statistics",
    "StaLlmGenerator per-second statistics",
    "[Final per-second]",
    "[Final overall]",
    "[Received Stats]",
)


class LiveTraceError(RuntimeError):
    """A path-bearing live verification failure."""


_OWNER_CREATION_TOKEN = object()
_DIRECTORY_OPEN_FLAGS = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC


def _same_identity(first, second):
    return (first.st_dev, first.st_ino) == (second.st_dev, second.st_ino)


def _delete_directory_contents_fd(directory_fd):
    """Delete entries beneath an opened directory without following links."""
    for name in os.listdir(directory_fd):
        try:
            entry_identity = os.stat(name, dir_fd=directory_fd, follow_symlinks=False)
        except FileNotFoundError:
            continue
        if not stat.S_ISDIR(entry_identity.st_mode):
            os.unlink(name, dir_fd=directory_fd)
            continue

        child_fd = os.open(name, _DIRECTORY_OPEN_FLAGS, dir_fd=directory_fd)
        try:
            child_identity = os.fstat(child_fd)
            if not _same_identity(entry_identity, child_identity):
                raise LiveTraceError(f"directory entry {name!r} changed before traversal")
            _delete_directory_contents_fd(child_fd)
            current_identity = os.stat(name, dir_fd=directory_fd, follow_symlinks=False)
            if not _same_identity(child_identity, current_identity):
                raise LiveTraceError(f"directory entry {name!r} changed during traversal")
        finally:
            os.close(child_fd)
        os.rmdir(name, dir_fd=directory_fd)


class OwnedTemporaryRun:
    """POSIX capability owning one exact temporary run directory identity."""

    __slots__ = (
        "path",
        "_name",
        "_parent_fd",
        "_child_fd",
        "_parent_device",
        "_parent_inode",
        "_device",
        "_inode",
        "_cleaned",
    )

    def __init__(self, path, name, parent_fd, child_fd, parent_identity, identity, token):
        if token is not _OWNER_CREATION_TOKEN:
            raise TypeError("OwnedTemporaryRun must be created with create()")
        self.path = path
        self._name = name
        self._parent_fd = parent_fd
        self._child_fd = child_fd
        self._parent_device = parent_identity.st_dev
        self._parent_inode = parent_identity.st_ino
        self._device = identity.st_dev
        self._inode = identity.st_ino
        self._cleaned = False

    @classmethod
    def create(cls, trace_name, temporary_parent=Path("/tmp")):
        """Create and take ownership of one exact directory identity."""
        safe_name = "".join(
            character if character.isalnum() or character in "-_" else "_"
            for character in str(trace_name)
        )
        if not safe_name:
            raise LiveTraceError(f"{temporary_parent}: empty temporary trace name")
        parent = Path(temporary_parent).resolve(strict=True)
        if not parent.is_dir():
            raise LiveTraceError(f"{temporary_parent}: temporary parent is not a directory")
        parent_fd = os.open(parent, _DIRECTORY_OPEN_FLAGS)
        child_fd = -1
        path = None
        try:
            parent_identity = os.fstat(parent_fd)
            path = Path(tempfile.mkdtemp(prefix=f"llm-trace-live.{safe_name}.", dir=parent))
            name = path.name
            entry_identity = os.stat(name, dir_fd=parent_fd, follow_symlinks=False)
            child_fd = os.open(name, _DIRECTORY_OPEN_FLAGS, dir_fd=parent_fd)
            child_identity = os.fstat(child_fd)
            if not stat.S_ISDIR(entry_identity.st_mode) or not _same_identity(
                entry_identity, child_identity
            ):
                raise LiveTraceError(f"{path}: created temporary run identity mismatch")
            return cls(
                path,
                name,
                parent_fd,
                child_fd,
                parent_identity,
                child_identity,
                _OWNER_CREATION_TOKEN,
            )
        except Exception:
            if child_fd >= 0:
                os.close(child_fd)
            if path is not None:
                try:
                    os.rmdir(path.name, dir_fd=parent_fd)
                except OSError:
                    pass
            os.close(parent_fd)
            raise

    def _close_fds(self):
        for attribute in ("_child_fd", "_parent_fd"):
            descriptor = getattr(self, attribute, -1)
            if descriptor >= 0:
                try:
                    os.close(descriptor)
                finally:
                    setattr(self, attribute, -1)

    def _quarantine_name(self):
        while True:
            name = f".llm-trace-live-quarantine.{secrets.token_hex(16)}"
            try:
                os.stat(name, dir_fd=self._parent_fd, follow_symlinks=False)
            except FileNotFoundError:
                return name

    def _restore_quarantine(self, quarantine_name):
        os.rename(
            quarantine_name,
            self._name,
            src_dir_fd=self._parent_fd,
            dst_dir_fd=self._parent_fd,
        )

    def cleanup(self):
        """Quarantine and delete only through retained matching directory FDs."""
        if self._cleaned:
            return
        if self._parent_fd < 0 or self._child_fd < 0:
            raise LiveTraceError(f"{self.path}: temporary-run ownership capability is closed")
        parent_identity = os.fstat(self._parent_fd)
        if (parent_identity.st_dev, parent_identity.st_ino) != (
            self._parent_device,
            self._parent_inode,
        ):
            self._close_fds()
            raise LiveTraceError(f"{self.path}: temporary parent identity mismatch")

        quarantine_name = self._quarantine_name()
        moved = False
        try:
            os.rename(
                self._name,
                quarantine_name,
                src_dir_fd=self._parent_fd,
                dst_dir_fd=self._parent_fd,
            )
            moved = True
            quarantine_identity = os.stat(
                quarantine_name, dir_fd=self._parent_fd, follow_symlinks=False
            )
            child_identity = os.fstat(self._child_fd)
            if (
                not stat.S_ISDIR(quarantine_identity.st_mode)
                or (quarantine_identity.st_dev, quarantine_identity.st_ino)
                != (self._device, self._inode)
                or not _same_identity(quarantine_identity, child_identity)
            ):
                raise LiveTraceError(
                    f"{self.path}: refusing cleanup after directory identity substitution"
                )

            _delete_directory_contents_fd(self._child_fd)
            final_identity = os.stat(
                quarantine_name, dir_fd=self._parent_fd, follow_symlinks=False
            )
            if not _same_identity(child_identity, final_identity):
                raise LiveTraceError(
                    f"{self.path}: refusing cleanup after quarantine identity substitution"
                )
            os.close(self._child_fd)
            self._child_fd = -1
            os.rmdir(quarantine_name, dir_fd=self._parent_fd)
            moved = False
            self._cleaned = True
        except Exception as error:
            restoration_error = None
            if moved:
                try:
                    self._restore_quarantine(quarantine_name)
                    moved = False
                except OSError as restore_error:
                    restoration_error = restore_error
            self._close_fds()
            if restoration_error is not None:
                raise LiveTraceError(
                    f"{self.path}: cleanup refused and quarantine restoration failed: "
                    f"{restoration_error}"
                ) from error
            if isinstance(error, LiveTraceError):
                raise
            raise LiveTraceError(f"{self.path}: cleanup failed safely: {error}") from error
        finally:
            if self._cleaned:
                self._close_fds()

    def __enter__(self):
        return self

    def __exit__(self, exception_type, exception, traceback):
        self.cleanup()

    def __del__(self):
        self._close_fds()


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
        raise LiveTraceError(f"{trace_directory}: policy/discovery mismatch: {'; '.join(details)}")
    return tuple(policy)


def build_llm_command(trace_path, run_directory, policy):
    """Build the one ns-3 command for a policy entry."""
    arguments = [
        "llm_sample",
        "--config",
        "contrib/llm/config/basic_config.toml",
        "--general-trace-file",
        str(trace_path),
        "--general-run-folder",
        str(run_directory),
    ]
    if policy.get("mode") == "fixed":
        arguments.extend(
            [
                "--simulation-duration-mode",
                "fixed",
                "--simulation-fixed-duration-seconds",
                str(policy.get("seconds")),
            ]
        )
    elif policy.get("mode") != "auto":
        raise LiveTraceError(f"{trace_path}: unknown live policy mode: {policy.get('mode')!r}")
    return ["./ns3", "run", shlex.join(arguments)]


def cleanup_run_directory(owner):
    """Remove a run directory only through its creating owner capability."""
    if type(owner) is not OwnedTemporaryRun:
        raise LiveTraceError(f"{owner}: refusing cleanup without temporary-run ownership")
    owner.cleanup()


def validate_output_document(
    document,
    source_path,
    expected_trace,
    expected_run_directory=None,
    expected_policy=None,
):
    """Validate one parsed output document and return its live metrics."""
    source_path = Path(source_path)
    _reject_removed_keys(document, source_path, "$")
    _expect_object_keys(document, ROOT_KEYS, source_path, "$")
    if type(document["schema_version"]) is not int or document["schema_version"] != 1:
        _fail(source_path, "$.schema_version", "expected integer 1")
    _expect_object_keys(
        document["measurement_semantics"], set(MEASUREMENT_SEMANTICS), source_path,
        "$.measurement_semantics"
    )
    for key, expected_value in MEASUREMENT_SEMANTICS.items():
        if document["measurement_semantics"][key] != expected_value or (
            expected_value is not None
            and type(document["measurement_semantics"][key]) is not str
        ):
            _fail(
                source_path,
                f"$.measurement_semantics.{key}",
                f"expected {expected_value!r}",
            )
    window_width = document["statistics_window_ms"]
    _expect_nonnegative_integer(window_width, source_path, "$.statistics_window_ms", positive=True)

    metadata = document["experiment_metadata"]
    _expect_object_keys(
        metadata, {"configuration", "entity_inventory"}, source_path,
        "$.experiment_metadata"
    )
    configuration = metadata["configuration"]
    _expect_object_keys(
        configuration, set(CONFIGURATION_KEYS), source_path,
        "$.experiment_metadata.configuration"
    )
    field_count = 0
    for section, expected_fields in CONFIGURATION_KEYS.items():
        section_path = f"$.experiment_metadata.configuration.{section}"
        _expect_object_keys(configuration[section], expected_fields, source_path, section_path)
        field_count += len(configuration[section])
    if len(configuration) != 8 or field_count != 36:
        _fail(
            source_path,
            "$.experiment_metadata.configuration",
            f"expected 8 sections and 36 fields, got {len(configuration)} and {field_count}",
        )
    _validate_configuration(configuration, source_path)
    if configuration["general"]["trace_file"] != str(expected_trace):
        _fail(
            source_path,
            "$.experiment_metadata.configuration.general.trace_file",
            f"expected {str(expected_trace)!r}",
        )
    if configuration["general"]["output_name"] != "output.json":
        _fail(
            source_path,
            "$.experiment_metadata.configuration.general.output_name",
            "expected default output.json",
        )
    if expected_run_directory is not None and configuration["general"]["run_folder"] != str(
        expected_run_directory
    ):
        _fail(
            source_path,
            "$.experiment_metadata.configuration.general.run_folder",
            f"expected {str(expected_run_directory)!r}",
        )
    if configuration["statistics"]["window_ms"] != window_width:
        _fail(
            source_path,
            "$.experiment_metadata.configuration.statistics.window_ms",
            "does not match statistics_window_ms",
        )
    if expected_policy is not None:
        expected_mode = expected_policy["mode"]
        if configuration["simulation"]["duration_mode"] != expected_mode:
            _fail(
                source_path,
                "$.experiment_metadata.configuration.simulation.duration_mode",
                f"expected {expected_mode!r}",
            )
        if (
            expected_mode == "fixed"
            and configuration["simulation"]["fixed_duration_seconds"]
            != expected_policy["seconds"]
        ):
            _fail(
                source_path,
                "$.experiment_metadata.configuration.simulation.fixed_duration_seconds",
                f"expected {expected_policy['seconds']!r}",
            )

    inventory = metadata["entity_inventory"]
    _expect_object_keys(
        inventory, {"access_points", "stations"}, source_path,
        "$.experiment_metadata.entity_inventory"
    )
    ap_inventory = _build_inventory(
        inventory["access_points"], "access_point", source_path,
        "$.experiment_metadata.entity_inventory.access_points"
    )
    sta_inventory = _build_inventory(
        inventory["stations"], "station", source_path,
        "$.experiment_metadata.entity_inventory.stations"
    )
    if not ap_inventory:
        _fail(
            source_path,
            "$.experiment_metadata.entity_inventory.access_points",
            "expected at least one access point",
        )
    if not sta_inventory:
        _fail(
            source_path,
            "$.experiment_metadata.entity_inventory.stations",
            "expected at least one station",
        )
    known_nodes = {
        identity[1]: record for identity, record in [*ap_inventory.items(), *sta_inventory.items()]
    }
    if len(known_nodes) != len(ap_inventory) + len(sta_inventory):
        _fail(
            source_path,
            "$.experiment_metadata.entity_inventory",
            "duplicate node_id across AP and STA inventories",
        )
    for identity, record in sta_inventory.items():
        if not any(ap_identity[0] == record["access_point_id"] for ap_identity in ap_inventory):
            _fail(
                source_path,
                "$.experiment_metadata.entity_inventory.stations",
                f"station {identity} references unknown access_point_id",
            )

    windows = document["windows"]
    _expect_list(windows, source_path, "$.windows")
    if not windows:
        _fail(source_path, "$.windows", "expected at least one sparse window")
    prior_index = -1
    for window_position, window in enumerate(windows):
        window_path = f"$.windows[{window_position}]"
        _expect_object_keys(window, WINDOW_KEYS, source_path, window_path)
        index = window["window_index"]
        if not _is_nonnegative_integer(index) or index <= prior_index:
            _fail(source_path, f"{window_path}.window_index", "expected increasing index")
        prior_index = index
        start = window["window_start_ms"]
        duration = window["window_duration_ms"]
        _expect_finite_number(start, source_path, f"{window_path}.window_start_ms")
        _expect_finite_number(
            duration, source_path, f"{window_path}.window_duration_ms", positive=True
        )
        if abs(start - index * window_width) > 1e-6:
            _fail(source_path, f"{window_path}.window_start_ms", "does not match index * width")
        if duration > window_width + 1e-6:
            _fail(
                source_path,
                f"{window_path}.window_duration_ms",
                "expected a positive full or partial configured window",
            )
        ap_entities = _validate_entity_array(
            window["access_points"], "access_point", ap_inventory, known_nodes,
            source_path, f"{window_path}.access_points"
        )
        sta_entities = _validate_entity_array(
            window["stations"], "station", sta_inventory, known_nodes,
            source_path, f"{window_path}.stations"
        )
        if not ap_entities:
            _fail(source_path, f"{window_path}.access_points", "missing active BSS parent")
        emitted_ap_ids = {entity["access_point_id"] for entity in ap_entities}
        for entity in sta_entities:
            if entity["access_point_id"] not in emitted_ap_ids:
                _fail(
                    source_path,
                    f"{window_path}.stations",
                    f"station node {entity['node_id']} lacks its AP BSS parent",
                )

    overall = document["overall"]
    _expect_object_keys(overall, {"access_points", "stations"}, source_path, "$.overall")
    overall_aps = _validate_entity_array(
        overall["access_points"], "access_point", ap_inventory, known_nodes,
        source_path, "$.overall.access_points"
    )
    overall_stas = _validate_entity_array(
        overall["stations"], "station", sta_inventory, known_nodes,
        source_path, "$.overall.stations"
    )
    if {_identity_key(entity, "access_point") for entity in overall_aps} != set(ap_inventory):
        _fail(source_path, "$.overall.access_points", "is not dense over AP inventory")
    if {_identity_key(entity, "station") for entity in overall_stas} != set(sta_inventory):
        _fail(source_path, "$.overall.stations", "is not dense over STA inventory")

    validation = document["validation"]
    _expect_object_keys(validation, VALIDATION_KEYS, source_path, "$.validation")
    for key in sorted(VALIDATION_KEYS):
        if type(validation[key]) is not bool or not validation[key]:
            _fail(source_path, f"$.validation.{key}", "expected true Boolean")

    return {
        "window_count": len(windows),
        "ap_inventory_count": len(ap_inventory),
        "sta_inventory_count": len(sta_inventory),
        "validation": dict(validation),
    }


def reject_legacy_console(console, source_path):
    """Reject removed final measurement banners and rows."""
    for marker in LEGACY_REPORT_MARKERS:
        if marker in console:
            raise LiveTraceError(f"{source_path}: console contains legacy report marker {marker!r}")


def load_output_document(
    output_path,
    expected_trace,
    expected_run_directory=None,
    expected_policy=None,
):
    """Parse and validate one output file."""
    output_path = Path(output_path)
    try:
        with output_path.open("r", encoding="utf-8") as output:
            document = json.load(output)
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise LiveTraceError(f"{output_path}: cannot parse output JSON: {error}") from error
    return validate_output_document(
        document, output_path, expected_trace, expected_run_directory, expected_policy
    )


def run_one_trace(
    outer_root,
    trace_path,
    policy,
    *,
    run_process=subprocess.run,
    temporary_parent=Path("/tmp"),
):
    """Validate and execute exactly one live trace, with unconditional cleanup."""
    outer_root = Path(outer_root).resolve()
    trace_path = Path(trace_path).resolve()
    try:
        relative_trace = trace_path.relative_to(outer_root)
    except ValueError as error:
        raise LiveTraceError(f"{trace_path}: trace is outside outer repository") from error
    timeout_seconds = policy["timeout_seconds"]
    validate_command = [
        "python3",
        "contrib/llm/scripts/find_window.py",
        "validate",
        str(relative_trace),
    ]
    validate_result = _run_captured(
        run_process, validate_command, outer_root, timeout_seconds, trace_path
    )
    if validate_result.returncode != 0:
        raise LiveTraceError(
            _format_run_failure(
                trace_path,
                validate_command,
                validate_result.returncode,
                validate_result.stdout,
                "trace validation command failed",
            )
        )

    with OwnedTemporaryRun.create(trace_path.stem, temporary_parent) as run_owner:
        run_directory = run_owner.path
        command = build_llm_command(relative_trace, run_directory, policy)
        started = time.monotonic()
        result = _run_captured(run_process, command, outer_root, timeout_seconds, trace_path)
        wall_time_seconds = time.monotonic() - started
        console = result.stdout
        if result.returncode != 0:
            raise LiveTraceError(
                _format_run_failure(
                    trace_path,
                    command,
                    result.returncode,
                    console,
                    "llm_sample command failed",
                )
            )
        try:
            reject_legacy_console(console, trace_path)
            output_path = run_directory / "output.json"
            metrics = load_output_document(
                output_path, relative_trace, run_directory, expected_policy=policy
            )
        except LiveTraceError as error:
            raise LiveTraceError(
                _format_run_failure(
                    trace_path,
                    command,
                    result.returncode,
                    console,
                    str(error),
                )
            ) from error
        metrics.update(
            {
                "policy": (
                    "auto" if policy["mode"] == "auto" else f"fixed-{policy['seconds']:.1f}s"
                ),
                "return_code": result.returncode,
                "wall_time_s": wall_time_seconds,
                "output_bytes": output_path.stat().st_size,
            }
        )
        return metrics


def _fail(source_path, json_path, message):
    raise LiveTraceError(f"{source_path}: {json_path}: {message}")


def _expect_object_keys(value, expected, source_path, json_path):
    if not isinstance(value, dict):
        _fail(source_path, json_path, "expected object")
    actual = set(value)
    if actual != set(expected):
        missing = sorted(set(expected) - actual)
        extra = sorted(actual - set(expected))
        details = []
        if missing:
            details.append("missing " + ", ".join(missing))
        if extra:
            details.append("unexpected " + ", ".join(extra))
        _fail(source_path, json_path, "wrong fields: " + "; ".join(details))


def _expect_list(value, source_path, json_path):
    if not isinstance(value, list):
        _fail(source_path, json_path, "expected array")


def _expect_nonnegative_integer(value, source_path, json_path, *, positive=False, maximum=None):
    if type(value) is not int or value < (1 if positive else 0):
        qualifier = "positive" if positive else "non-negative"
        _fail(source_path, json_path, f"expected {qualifier} integer")
    if maximum is not None and value > maximum:
        _fail(source_path, json_path, f"expected integer no greater than {maximum}")


def _expect_finite_number(
    value,
    source_path,
    json_path,
    *,
    positive=False,
    maximum=None,
):
    is_finite_number = type(value) is int or (type(value) is float and math.isfinite(value))
    if not is_finite_number or value < 0:
        qualifier = "positive " if positive else "non-negative "
        _fail(source_path, json_path, f"expected finite {qualifier}number")
    if positive and value <= 0:
        _fail(source_path, json_path, "expected finite positive number")
    if maximum is not None and value > maximum:
        _fail(source_path, json_path, f"expected number no greater than {maximum}")


def _expect_optional_finite_number(value, source_path, json_path, *, maximum=None):
    if value is None:
        return
    _expect_finite_number(value, source_path, json_path, maximum=maximum)


def _expect_optional_nonnegative_integer(value, source_path, json_path):
    if value is None:
        return
    _expect_nonnegative_integer(value, source_path, json_path)


def _expect_string(value, source_path, json_path, *, allow_none=False, allowed=None):
    if allow_none and value is None:
        return
    if type(value) is not str or not value:
        _fail(source_path, json_path, "expected nonempty string")
    if allowed is not None and value not in allowed:
        _fail(source_path, json_path, "expected one of " + ", ".join(sorted(allowed)))


def _expect_boolean(value, source_path, json_path):
    if type(value) is not bool:
        _fail(source_path, json_path, "expected Boolean")


def _validate_integer_fields(value, fields, source_path, json_path):
    for field in fields:
        _expect_nonnegative_integer(value[field], source_path, f"{json_path}.{field}")


def _validate_optional_number_fields(value, fields, source_path, json_path, *, maximum=None):
    for field in fields:
        _expect_optional_finite_number(
            value[field], source_path, f"{json_path}.{field}", maximum=maximum
        )


def _validate_configuration(configuration, source_path):
    base = "$.experiment_metadata.configuration"
    general = configuration["general"]
    _expect_string(general["trace_file"], source_path, f"{base}.general.trace_file")
    _expect_string(
        general["run_folder"], source_path, f"{base}.general.run_folder", allow_none=True
    )
    _expect_string(general["output_name"], source_path, f"{base}.general.output_name")

    simulation = configuration["simulation"]
    _expect_string(
        simulation["duration_mode"],
        source_path,
        f"{base}.simulation.duration_mode",
        allowed={"auto", "fixed"},
    )
    _expect_finite_number(
        simulation["fixed_duration_seconds"],
        source_path,
        f"{base}.simulation.fixed_duration_seconds",
    )
    if simulation["duration_mode"] == "fixed" and simulation["fixed_duration_seconds"] <= 0:
        _fail(
            source_path,
            f"{base}.simulation.fixed_duration_seconds",
            "expected positive value in fixed mode",
        )
    _expect_finite_number(
        simulation["auto_tail_seconds"],
        source_path,
        f"{base}.simulation.auto_tail_seconds",
    )
    _expect_nonnegative_integer(
        simulation["rng_seed"],
        source_path,
        f"{base}.simulation.rng_seed",
        positive=True,
        maximum=4294944442,
    )
    _expect_nonnegative_integer(simulation["rng_run"], source_path, f"{base}.simulation.rng_run")

    topology = configuration["topology"]
    _expect_nonnegative_integer(
        topology["bss_count"], source_path, f"{base}.topology.bss_count", positive=True, maximum=256
    )
    _expect_nonnegative_integer(
        topology["stations_per_bss"],
        source_path,
        f"{base}.topology.stations_per_bss",
        positive=True,
        maximum=253,
    )
    for field in ("bss_spacing_m", "station_radius_m", "generator_start_seconds"):
        _expect_finite_number(topology[field], source_path, f"{base}.topology.{field}")
    _expect_boolean(
        topology["isolate_bss_channels"], source_path, f"{base}.topology.isolate_bss_channels"
    )
    _expect_string(topology["ssid_prefix"], source_path, f"{base}.topology.ssid_prefix")
    for field in ("ap_sink_port", "station_sink_base_port"):
        _expect_nonnegative_integer(
            topology[field], source_path, f"{base}.topology.{field}", positive=True, maximum=65535
        )

    distribution = configuration["distribution"]
    _expect_nonnegative_integer(
        distribution["max_agents_per_station"],
        source_path,
        f"{base}.distribution.max_agents_per_station",
    )
    _expect_boolean(
        distribution["low_contention_priority"],
        source_path,
        f"{base}.distribution.low_contention_priority",
    )
    _expect_nonnegative_integer(
        distribution["slot_ms"],
        source_path,
        f"{base}.distribution.slot_ms",
        positive=True,
    )

    wifi = configuration["wifi"]
    _expect_string(
        wifi["band"], source_path, f"{base}.wifi.band", allowed={"2.4GHz", "5GHz", "6GHz"}
    )
    _expect_nonnegative_integer(
        wifi["channel_number"], source_path, f"{base}.wifi.channel_number", maximum=65535
    )
    _expect_nonnegative_integer(
        wifi["bandwidth_mhz"], source_path, f"{base}.wifi.bandwidth_mhz", positive=True
    )
    if wifi["bandwidth_mhz"] not in {20, 40, 80, 160}:
        _fail(source_path, f"{base}.wifi.bandwidth_mhz", "expected 20, 40, 80, or 160")
    _expect_nonnegative_integer(
        wifi["primary_20_index"], source_path, f"{base}.wifi.primary_20_index", maximum=255
    )
    if wifi["primary_20_index"] >= wifi["bandwidth_mhz"] // 20:
        _fail(source_path, f"{base}.wifi.primary_20_index", "outside configured bandwidth")
    _expect_string(wifi["rate_manager"], source_path, f"{base}.wifi.rate_manager")
    _expect_boolean(wifi["active_probing"], source_path, f"{base}.wifi.active_probing")

    tcp = configuration["tcp"]
    _expect_string(tcp["congestion_control"], source_path, f"{base}.tcp.congestion_control")
    for field in ("segment_size_bytes", "send_buffer_bytes", "receive_buffer_bytes"):
        _expect_nonnegative_integer(
            tcp[field], source_path, f"{base}.tcp.{field}", positive=True
        )

    _expect_nonnegative_integer(
        configuration["statistics"]["window_ms"],
        source_path,
        f"{base}.statistics.window_ms",
        positive=True,
    )
    log_levels = {"off", "error", "warn", "info", "debug", "function", "logic", "all"}
    for field, value in configuration["logging"].items():
        _expect_string(value, source_path, f"{base}.logging.{field}", allowed=log_levels)


def _is_nonnegative_integer(value):
    return type(value) is int and value >= 0


def _reject_removed_keys(value, source_path, json_path):
    if isinstance(value, dict):
        for key, child in value.items():
            child_path = f"{json_path}.{key}"
            if key in REMOVED_KEYS:
                _fail(source_path, child_path, f"removed field {key!r} remains")
            _reject_removed_keys(child, source_path, child_path)
    elif isinstance(value, list):
        for index, child in enumerate(value):
            _reject_removed_keys(child, source_path, f"{json_path}[{index}]")


def _identity_key(record, kind):
    if kind == "access_point":
        return record["access_point_id"], record["node_id"]
    return record["access_point_id"], record["node_id"], record["station_index"]


def _validate_identity(record, kind, source_path, json_path, include_categories=False):
    identity_keys = AP_IDENTITY_KEYS if kind == "access_point" else STA_IDENTITY_KEYS
    expected_keys = identity_keys | CATEGORY_KEYS if include_categories else identity_keys
    _expect_object_keys(record, expected_keys, source_path, json_path)
    for key in identity_keys & {"access_point_id", "station_index", "node_id"}:
        _expect_nonnegative_integer(record[key], source_path, f"{json_path}.{key}")
    for key in {"node_label", "ipv4"}:
        _expect_string(record[key], source_path, f"{json_path}.{key}")
    return _identity_key(record, kind)


def _build_inventory(records, kind, source_path, json_path):
    _expect_list(records, source_path, json_path)
    inventory = {}
    node_ids = set()
    for index, record in enumerate(records):
        record_path = f"{json_path}[{index}]"
        identity = _validate_identity(record, kind, source_path, record_path)
        if identity in inventory:
            _fail(source_path, record_path, f"duplicate inventory identity {identity}")
        if record["node_id"] in node_ids:
            _fail(source_path, record_path, f"duplicate inventory node_id {record['node_id']}")
        inventory[identity] = record
        node_ids.add(record["node_id"])
    return inventory


def _validate_sample_distribution(value, source_path, json_path):
    _expect_object_keys(value, SAMPLE_KEYS, source_path, json_path)
    _expect_nonnegative_integer(value["sample_count"], source_path, f"{json_path}.sample_count")
    derived_fields = {"average_us", "standard_deviation_us", "minimum_us", "maximum_us"}
    _validate_optional_number_fields(value, derived_fields, source_path, json_path)
    if value["sample_count"] == 0 and any(value[field] is not None for field in derived_fields):
        _fail(source_path, json_path, "zero-sample distribution has derived values")
    if value["sample_count"] > 0 and any(value[field] is None for field in derived_fields):
        _fail(source_path, json_path, "sampled distribution has null derived values")


def _validate_peer(peer, expected_keys, known_nodes, source_path, json_path):
    _expect_object_keys(peer, expected_keys, source_path, json_path)
    peer_node_id = peer["peer_node_id"]
    if not _is_nonnegative_integer(peer_node_id) or peer_node_id not in known_nodes:
        _fail(source_path, f"{json_path}.peer_node_id", "does not reference entity inventory")
    _expect_string(peer["peer_ipv4"], source_path, f"{json_path}.peer_ipv4")
    if peer["peer_ipv4"] != known_nodes[peer_node_id]["ipv4"]:
        _fail(source_path, f"{json_path}.peer_ipv4", "does not match inventory")


def _validate_reason_array(reasons, source_path, json_path):
    _expect_list(reasons, source_path, json_path)
    for index, reason in enumerate(reasons):
        reason_path = f"{json_path}[{index}]"
        _expect_object_keys(reason, MAC_REASON_KEYS, source_path, reason_path)
        _validate_integer_fields(reason, MAC_REASON_KEYS, source_path, reason_path)


def _validate_general_direction(value, source_path, json_path):
    _validate_integer_fields(
        value,
        {
            "estimated_transmitted_tcp_payload_bytes",
            "estimated_matched_tcp_payload_bytes",
            "matched_packet_count",
            "total_transmission_duration_us",
        },
        source_path,
        json_path,
    )
    _validate_optional_number_fields(
        value,
        {
            "average_transmission_duration_us",
            "transmission_duration_standard_deviation_us",
            "minimum_transmission_duration_us",
            "maximum_transmission_duration_us",
            "effective_throughput_mbps",
        },
        source_path,
        json_path,
    )
    _validate_sample_distribution(
        value["application_to_phy_delay"], source_path, f"{json_path}.application_to_phy_delay"
    )


def _validate_app_agent(agent, source_path, json_path):
    _expect_object_keys(agent, APP_AGENT_KEYS, source_path, json_path)
    _expect_string(agent["agent_key"], source_path, f"{json_path}.agent_key")
    _validate_integer_fields(
        agent,
        {
            "accepted_send_count",
            "accepted_payload_bytes",
            "drop_event_count",
            "dropped_payload_bytes",
        },
        source_path,
        json_path,
    )
    _validate_optional_number_fields(
        agent, {"accepted_throughput_mbps"}, source_path, json_path
    )
    _validate_optional_number_fields(
        agent, {"accepted_bandwidth_share_percent"}, source_path, json_path, maximum=100.0
    )


def _validate_app_peer(peer, known_nodes, source_path, json_path):
    _validate_peer(peer, APP_PEER_KEYS, known_nodes, source_path, json_path)
    _validate_integer_fields(
        peer,
        {
            "accepted_send_count",
            "accepted_payload_bytes",
            "receive_event_count",
            "received_payload_bytes",
            "drop_event_count",
            "dropped_payload_bytes",
        },
        source_path,
        json_path,
    )
    _validate_optional_number_fields(
        peer,
        {"accepted_throughput_mbps", "received_throughput_mbps"},
        source_path,
        json_path,
    )
    _validate_optional_number_fields(
        peer,
        {"accepted_bandwidth_share_percent", "received_bandwidth_share_percent"},
        source_path,
        json_path,
        maximum=100.0,
    )


def _validate_app_direction(value, known_nodes, source_path, json_path):
    _validate_integer_fields(
        value,
        {
            "accepted_send_count",
            "accepted_payload_bytes",
            "receive_event_count",
            "received_payload_bytes",
            "drop_event_count",
            "dropped_payload_bytes",
        },
        source_path,
        json_path,
    )
    _validate_optional_number_fields(
        value,
        {"accepted_throughput_mbps", "received_throughput_mbps"},
        source_path,
        json_path,
    )
    _validate_sample_distribution(
        value["receive_interarrival_time"], source_path, f"{json_path}.receive_interarrival_time"
    )
    _expect_list(value["agents"], source_path, f"{json_path}.agents")
    for index, agent in enumerate(value["agents"]):
        _validate_app_agent(agent, source_path, f"{json_path}.agents[{index}]")
    _expect_list(value["peers"], source_path, f"{json_path}.peers")
    for index, peer in enumerate(value["peers"]):
        _validate_app_peer(peer, known_nodes, source_path, f"{json_path}.peers[{index}]")


def _validate_tcp_direction(value, known_nodes, source_path, json_path):
    _expect_list(value["connections"], source_path, f"{json_path}.connections")
    for index, connection in enumerate(value["connections"]):
        connection_path = f"{json_path}.connections[{index}]"
        _validate_peer(connection, TCP_CONNECTION_KEYS, known_nodes, source_path, connection_path)
        _expect_nonnegative_integer(
            connection["congestion_window_observation_duration_us"],
            source_path,
            f"{connection_path}.congestion_window_observation_duration_us",
        )
        _expect_optional_finite_number(
            connection["average_congestion_window_bytes"],
            source_path,
            f"{connection_path}.average_congestion_window_bytes",
        )
        _expect_optional_nonnegative_integer(
            connection["last_congestion_window_bytes"],
            source_path,
            f"{connection_path}.last_congestion_window_bytes",
        )
        _validate_sample_distribution(
            connection["round_trip_time"], source_path, f"{connection_path}.round_trip_time"
        )


def _validate_mac_peer(peer, known_nodes, source_path, json_path):
    _validate_peer(peer, MAC_PEER_KEYS, known_nodes, source_path, json_path)
    _validate_integer_fields(
        peer,
        {
            "estimated_transmit_event_count",
            "estimated_transmitted_tcp_payload_bytes",
            "estimated_receive_event_count",
            "estimated_received_tcp_payload_bytes",
            "mpdu_drop_count",
            "mpdu_drop_bytes",
            "data_failure_count",
            "final_data_failure_count",
        },
        source_path,
        json_path,
    )
    _validate_optional_number_fields(
        peer,
        {"estimated_transmit_throughput_mbps", "estimated_receive_throughput_mbps"},
        source_path,
        json_path,
    )
    _validate_reason_array(
        peer["mpdu_drops_by_reason"], source_path, f"{json_path}.mpdu_drops_by_reason"
    )


def _validate_mac_direction(value, known_nodes, source_path, json_path):
    _validate_integer_fields(
        value,
        {
            "estimated_transmit_event_count",
            "estimated_transmitted_tcp_payload_bytes",
            "estimated_receive_event_count",
            "estimated_received_tcp_payload_bytes",
            "transmit_drop_count",
            "transmit_drop_packet_bytes",
            "mpdu_drop_count",
            "mpdu_drop_bytes",
            "data_failure_count",
            "final_data_failure_count",
        },
        source_path,
        json_path,
    )
    _validate_optional_number_fields(
        value,
        {"estimated_transmit_throughput_mbps", "estimated_receive_throughput_mbps"},
        source_path,
        json_path,
    )
    _validate_reason_array(
        value["mpdu_drops_by_reason"], source_path, f"{json_path}.mpdu_drops_by_reason"
    )
    _expect_list(value["peers"], source_path, f"{json_path}.peers")
    for index, peer in enumerate(value["peers"]):
        _validate_mac_peer(peer, known_nodes, source_path, f"{json_path}.peers[{index}]")


def _validate_phy_peer(peer, known_nodes, source_path, json_path):
    _validate_peer(peer, PHY_PEER_KEYS, known_nodes, source_path, json_path)
    _validate_integer_fields(
        peer,
        {
            "tagged_payload_bytes",
            "unique_tagged_payload_bytes",
            "transmission_attempt_count",
            "retransmission_count",
        },
        source_path,
        json_path,
    )
    _expect_finite_number(
        peer["transmission_airtime_us"], source_path, f"{json_path}.transmission_airtime_us"
    )
    _validate_optional_number_fields(
        peer, {"average_data_rate_mbps", "throughput_mbps"}, source_path, json_path
    )


def _validate_phy_direction(value, known_nodes, source_path, json_path):
    _validate_integer_fields(
        value,
        {
            "tagged_payload_bytes",
            "unique_tagged_payload_bytes",
            "tagged_mpdu_count",
            "complete_tagged_mpdu_bytes",
            "transmission_attempt_count",
            "retransmission_count",
        },
        source_path,
        json_path,
    )
    _expect_finite_number(
        value["transmission_airtime_us"], source_path, f"{json_path}.transmission_airtime_us"
    )
    _validate_optional_number_fields(
        value, {"average_data_rate_mbps", "throughput_mbps"}, source_path, json_path
    )
    _expect_list(value["peers"], source_path, f"{json_path}.peers")
    for index, peer in enumerate(value["peers"]):
        _validate_phy_peer(peer, known_nodes, source_path, f"{json_path}.peers[{index}]")


def _validate_entity(record, kind, inventory, known_nodes, source_path, json_path):
    identity = _validate_identity(record, kind, source_path, json_path, include_categories=True)
    if identity not in inventory:
        _fail(source_path, json_path, f"entity identity {identity} does not reference inventory")
    inventory_identity = inventory[identity]
    for key in AP_IDENTITY_KEYS | ({"station_index"} if kind == "station" else set()):
        if record[key] != inventory_identity[key]:
            _fail(source_path, f"{json_path}.{key}", "does not match inventory")

    for category_name, expected_direction_keys in (
        ("general_stats", GENERAL_KEYS),
        ("app_stats", APP_KEYS),
        ("tcp_stats", TCP_KEYS),
        ("mac_stats", MAC_KEYS),
    ):
        category_path = f"{json_path}.{category_name}"
        category = record[category_name]
        _expect_object_keys(category, DIRECTIONS, source_path, category_path)
        for direction in sorted(DIRECTIONS):
            direction_path = f"{category_path}.{direction}"
            value = category[direction]
            _expect_object_keys(value, expected_direction_keys, source_path, direction_path)
            if category_name == "general_stats":
                _validate_general_direction(value, source_path, direction_path)
            elif category_name == "app_stats":
                _validate_app_direction(value, known_nodes, source_path, direction_path)
            elif category_name == "tcp_stats":
                _validate_tcp_direction(value, known_nodes, source_path, direction_path)
            else:
                _validate_mac_direction(value, known_nodes, source_path, direction_path)

    phy = record["phy_stats"]
    phy_path = f"{json_path}.phy_stats"
    _expect_object_keys(phy, PHY_KEYS, source_path, phy_path)
    _expect_nonnegative_integer(phy["busy_time_us"], source_path, f"{phy_path}.busy_time_us")
    _expect_optional_finite_number(
        phy["channel_utilization_percent"],
        source_path,
        f"{phy_path}.channel_utilization_percent",
        maximum=100.0,
    )
    for direction in sorted(DIRECTIONS):
        direction_path = f"{phy_path}.{direction}"
        value = phy[direction]
        _expect_object_keys(value, PHY_DIRECTION_KEYS, source_path, direction_path)
        _validate_phy_direction(value, known_nodes, source_path, direction_path)
    return identity


def _validate_entity_array(records, kind, inventory, known_nodes, source_path, json_path):
    _expect_list(records, source_path, json_path)
    identities = set()
    for index, record in enumerate(records):
        identity = _validate_entity(
            record, kind, inventory, known_nodes, source_path, f"{json_path}[{index}]"
        )
        if identity in identities:
            _fail(source_path, f"{json_path}[{index}]", f"duplicate entity identity {identity}")
        identities.add(identity)
    return records


def _console_text(value):
    if value is None:
        return ""
    if isinstance(value, bytes):
        return value.decode("utf-8", errors="replace")
    return str(value)


def _run_captured(run_process, command, cwd, timeout_seconds, trace_path):
    try:
        result = run_process(
            command,
            cwd=cwd,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
            check=False,
            timeout=timeout_seconds,
        )
    except subprocess.TimeoutExpired as error:
        console = _console_text(error.stdout)
        raise LiveTraceError(
            _format_run_failure(
                trace_path, command, "timeout", console,
                f"command exceeded {timeout_seconds} seconds"
            )
        ) from error
    result.stdout = _console_text(result.stdout)
    return result


def _format_run_failure(trace_path, command, return_code, console, reason):
    lines = console.splitlines()
    tail = "\n".join(lines[-200:])
    return (
        f"{trace_path}: {reason}\n"
        f"trace: {trace_path}\n"
        f"command: {shlex.join(str(part) for part in command)}\n"
        f"return code: {return_code}\n"
        f"last 200 console lines:\n{tail}"
    )


def _sample_distribution():
    return {
        "sample_count": 0,
        "average_us": None,
        "standard_deviation_us": None,
        "minimum_us": None,
        "maximum_us": None,
    }


def _entity(access_point_id=0, station_index=None, node_id=1, ipv4="10.1.0.1"):
    general = {
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
    app = {
        "accepted_send_count": 0,
        "accepted_payload_bytes": 0,
        "accepted_throughput_mbps": 0.0,
        "receive_event_count": 0,
        "received_payload_bytes": 0,
        "received_throughput_mbps": 0.0,
        "drop_event_count": 0,
        "dropped_payload_bytes": 0,
        "receive_interarrival_time": _sample_distribution(),
        "agents": [],
        "peers": [],
    }
    tcp = {"connections": []}
    mac = {
        "estimated_transmit_event_count": 0,
        "estimated_transmitted_tcp_payload_bytes": 0,
        "estimated_transmit_throughput_mbps": 0.0,
        "estimated_receive_event_count": 0,
        "estimated_received_tcp_payload_bytes": 0,
        "estimated_receive_throughput_mbps": 0.0,
        "transmit_drop_count": 0,
        "transmit_drop_packet_bytes": 0,
        "mpdu_drop_count": 0,
        "mpdu_drop_bytes": 0,
        "data_failure_count": 0,
        "final_data_failure_count": 0,
        "mpdu_drops_by_reason": [],
        "peers": [],
    }
    phy_direction = {
        "tagged_payload_bytes": 0,
        "unique_tagged_payload_bytes": 0,
        "tagged_mpdu_count": 0,
        "complete_tagged_mpdu_bytes": 0,
        "transmission_attempt_count": 0,
        "retransmission_count": 0,
        "transmission_airtime_us": 0.0,
        "average_data_rate_mbps": None,
        "throughput_mbps": 0.0,
        "peers": [],
    }
    result = {
        "access_point_id": access_point_id,
        "node_id": node_id,
        "node_label": f"node-{node_id}",
        "ipv4": ipv4,
        "general_stats": {"uplink": copy.deepcopy(general), "downlink": copy.deepcopy(general)},
        "app_stats": {"uplink": copy.deepcopy(app), "downlink": copy.deepcopy(app)},
        "tcp_stats": {"uplink": copy.deepcopy(tcp), "downlink": copy.deepcopy(tcp)},
        "mac_stats": {"uplink": copy.deepcopy(mac), "downlink": copy.deepcopy(mac)},
        "phy_stats": {
            "busy_time_us": 0,
            "channel_utilization_percent": 0.0,
            "uplink": copy.deepcopy(phy_direction),
            "downlink": copy.deepcopy(phy_direction),
        },
    }
    if station_index is not None:
        result["station_index"] = station_index
    return result


def _populate_nested_entity(entity, peer_node_id, peer_ipv4):
    app = entity["app_stats"]["uplink"]
    app["agents"] = [{
        "agent_key": "agent-1",
        "accepted_send_count": 1,
        "accepted_payload_bytes": 100,
        "accepted_throughput_mbps": 0.08,
        "accepted_bandwidth_share_percent": 100.0,
        "drop_event_count": 0,
        "dropped_payload_bytes": 0,
    }]
    app["peers"] = [{
        "peer_node_id": peer_node_id,
        "peer_ipv4": peer_ipv4,
        "accepted_send_count": 1,
        "accepted_payload_bytes": 100,
        "accepted_throughput_mbps": 0.08,
        "accepted_bandwidth_share_percent": 100.0,
        "receive_event_count": 1,
        "received_payload_bytes": 90,
        "received_throughput_mbps": 0.072,
        "received_bandwidth_share_percent": 100.0,
        "drop_event_count": 0,
        "dropped_payload_bytes": 0,
    }]
    entity["tcp_stats"]["uplink"]["connections"] = [{
        "peer_node_id": peer_node_id,
        "peer_ipv4": peer_ipv4,
        "congestion_window_observation_duration_us": 10000,
        "average_congestion_window_bytes": 2048.0,
        "last_congestion_window_bytes": 4096,
        "round_trip_time": {
            "sample_count": 1,
            "average_us": 100.0,
            "standard_deviation_us": 0.0,
            "minimum_us": 100.0,
            "maximum_us": 100.0,
        },
    }]
    mac = entity["mac_stats"]["uplink"]
    mac["mpdu_drops_by_reason"] = [{"reason_code": 1, "drop_count": 1}]
    mac["peers"] = [{
        "peer_node_id": peer_node_id,
        "peer_ipv4": peer_ipv4,
        "estimated_transmit_event_count": 1,
        "estimated_transmitted_tcp_payload_bytes": 100,
        "estimated_transmit_throughput_mbps": 0.08,
        "estimated_receive_event_count": 1,
        "estimated_received_tcp_payload_bytes": 90,
        "estimated_receive_throughput_mbps": 0.072,
        "mpdu_drop_count": 1,
        "mpdu_drop_bytes": 100,
        "data_failure_count": 1,
        "final_data_failure_count": 0,
        "mpdu_drops_by_reason": [{"reason_code": 1, "drop_count": 1}],
    }]
    entity["phy_stats"]["uplink"]["peers"] = [{
        "peer_node_id": peer_node_id,
        "peer_ipv4": peer_ipv4,
        "tagged_payload_bytes": 100,
        "unique_tagged_payload_bytes": 100,
        "transmission_attempt_count": 1,
        "retransmission_count": 0,
        "transmission_airtime_us": 10.0,
        "average_data_rate_mbps": 80.0,
        "throughput_mbps": 0.08,
    }]


def _valid_document(trace_path, run_directory="/tmp/llm-trace-live.test.random"):
    ap = _entity()
    sta = _entity(station_index=0, node_id=2, ipv4="10.1.0.2")
    _populate_nested_entity(ap, 2, "10.1.0.2")
    _populate_nested_entity(sta, 1, "10.1.0.1")
    configuration = {
        "general": {
            "trace_file": trace_path,
            "run_folder": run_directory,
            "output_name": "output.json",
        },
        "simulation": {
            "duration_mode": "auto",
            "fixed_duration_seconds": 0.0,
            "auto_tail_seconds": 2.0,
            "rng_seed": 12345,
            "rng_run": 1,
        },
        "topology": {
            "bss_count": 1,
            "stations_per_bss": 1,
            "bss_spacing_m": 100.0,
            "station_radius_m": 5.0,
            "isolate_bss_channels": True,
            "ssid_prefix": "llm-ap-",
            "ap_sink_port": 10000,
            "station_sink_base_port": 9000,
            "generator_start_seconds": 1.0,
        },
        "distribution": {
            "max_agents_per_station": 832,
            "low_contention_priority": True,
            "slot_ms": 10,
        },
        "wifi": {
            "band": "5GHz",
            "channel_number": 0,
            "bandwidth_mhz": 20,
            "primary_20_index": 0,
            "rate_manager": "ns3::MinstrelHtWifiManager",
            "active_probing": True,
        },
        "tcp": {
            "congestion_control": "ns3::TcpHighSpeed",
            "segment_size_bytes": 1460,
            "send_buffer_bytes": 33554432,
            "receive_buffer_bytes": 33554432,
        },
        "statistics": {"window_ms": 10},
        "logging": {
            "sample_scenario_level": "info",
            "ap_generator_level": "warn",
            "sta_generator_level": "warn",
            "traffic_sink_level": "warn",
            "contention_distribution_level": "info",
        },
    }
    return {
        "schema_version": 1,
        "measurement_semantics": {
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
        },
        "statistics_window_ms": 10,
        "windows": [{
            "window_index": 0,
            "window_start_ms": 0.0,
            "window_duration_ms": 10.0,
            "access_points": [copy.deepcopy(ap)],
            "stations": [copy.deepcopy(sta)],
        }],
        "overall": {"access_points": [copy.deepcopy(ap)], "stations": [copy.deepcopy(sta)]},
        "validation": {key: True for key in VALIDATION_KEYS},
        "experiment_metadata": {
            "configuration": configuration,
            "entity_inventory": {
                "access_points": [{
                    "access_point_id": 0,
                    "node_id": 1,
                    "node_label": "node-1",
                    "ipv4": "10.1.0.1",
                }],
                "stations": [{
                    "access_point_id": 0,
                    "station_index": 0,
                    "node_id": 2,
                    "node_label": "node-2",
                    "ipv4": "10.1.0.2",
                }],
            },
        },
    }


class LiveTraceSelfTest(unittest.TestCase):
    def setUp(self):
        self.trace = "contrib/llm/traces/1W_high_load_1s.json"
        self.source = Path("/tmp/fake/output.json")

    def assert_path_error(self, function, *args, text):
        with self.assertRaisesRegex(LiveTraceError, str(self.source)) as context:
            function(*args)
        self.assertIn(text, str(context.exception))

    def assert_document_error(self, document, json_path):
        self.assert_path_error(
            validate_output_document,
            document,
            self.source,
            self.trace,
            text=json_path,
        )

    def test_policy_coverage_accepts_exact_set(self):
        directory = Path("/workspace/contrib/llm/traces")
        discovered = [directory / name for name in reversed(POLICY)]
        self.assertEqual(validate_policy_coverage(discovered, directory), tuple(POLICY))

    def test_policy_coverage_rejects_missing_and_unknown(self):
        directory = Path("/workspace/contrib/llm/traces")
        missing = [directory / name for name in POLICY if name != "1W_high_load_1s.json"]
        with self.assertRaisesRegex(LiveTraceError, "1W_high_load_1s.json"):
            validate_policy_coverage(missing, directory)
        unknown = [directory / name for name in POLICY] + [directory / "new.json"]
        with self.assertRaisesRegex(LiveTraceError, "new.json"):
            validate_policy_coverage(unknown, directory)

    def test_builds_all_four_exact_commands(self):
        run_directory = Path("/tmp/llm-trace-live.test.random")
        for name, policy in POLICY.items():
            expected_inner = (
                "llm_sample --config contrib/llm/config/basic_config.toml "
                f"--general-trace-file contrib/llm/traces/{name} "
                f"--general-run-folder {run_directory}"
            )
            if policy["mode"] == "fixed":
                expected_inner += (
                    " --simulation-duration-mode fixed "
                    "--simulation-fixed-duration-seconds 1.0"
                )
            self.assertEqual(
                build_llm_command(Path("contrib/llm/traces") / name, run_directory, policy),
                ["./ns3", "run", expected_inner],
            )

    def test_validates_exact_document(self):
        metrics = validate_output_document(
            _valid_document(self.trace), self.source, self.trace,
            Path("/tmp/llm-trace-live.test.random")
        )
        self.assertEqual(metrics["window_count"], 1)
        self.assertEqual(metrics["ap_inventory_count"], 1)
        self.assertEqual(metrics["sta_inventory_count"], 1)

    def test_accepts_null_optional_averages(self):
        document = _valid_document(self.trace)
        general = document["windows"][0]["access_points"][0]["general_stats"]["downlink"]
        self.assertIsNone(general["average_transmission_duration_us"])
        self.assertIsNone(general["effective_throughput_mbps"])
        distribution = general["application_to_phy_delay"]
        self.assertEqual(distribution["sample_count"], 0)
        self.assertIsNone(distribution["average_us"])
        validate_output_document(document, self.source, self.trace)

    def test_rejects_non_integer_schema_versions(self):
        for invalid in (True, 1.0):
            with self.subTest(invalid=invalid):
                document = _valid_document(self.trace)
                document["schema_version"] = invalid
                self.assert_document_error(document, "$.schema_version")

    def test_rejects_wrong_measurement_semantics_types_and_values(self):
        document = _valid_document(self.trace)
        document["measurement_semantics"]["access_point_role"] = 7
        self.assert_document_error(document, "$.measurement_semantics.access_point_role")

        document = _valid_document(self.trace)
        document["measurement_semantics"]["access_point_role"] = "physical AP only"
        self.assert_document_error(document, "$.measurement_semantics.access_point_role")

    def test_rejects_invalid_required_scalar_types(self):
        document = _valid_document(self.trace)
        app = document["windows"][0]["access_points"][0]["app_stats"]["uplink"]
        app["accepted_payload_bytes"] = "100"
        self.assert_document_error(
            document,
            "$.windows[0].access_points[0].app_stats.uplink.accepted_payload_bytes",
        )

        document = _valid_document(self.trace)
        app = document["windows"][0]["access_points"][0]["app_stats"]["uplink"]
        app["accepted_send_count"] = True
        self.assert_document_error(
            document,
            "$.windows[0].access_points[0].app_stats.uplink.accepted_send_count",
        )

        document = _valid_document(self.trace)
        general = document["windows"][0]["access_points"][0]["general_stats"]["uplink"]
        general["matched_packet_count"] = None
        self.assert_document_error(
            document,
            "$.windows[0].access_points[0].general_stats.uplink.matched_packet_count",
        )

        document = _valid_document(self.trace)
        document["validation"]["overall_matches_windows"] = 1
        self.assert_document_error(document, "$.validation.overall_matches_windows")

    def test_rejects_nonfinite_numbers(self):
        for invalid in (float("inf"), float("-inf"), float("nan")):
            with self.subTest(invalid=invalid):
                document = _valid_document(self.trace)
                document["windows"][0]["window_start_ms"] = invalid
                self.assert_document_error(document, "$.windows[0].window_start_ms")

        document = _valid_document(self.trace)
        phy = document["windows"][0]["access_points"][0]["phy_stats"]["uplink"]
        phy["average_data_rate_mbps"] = float("inf")
        self.assert_document_error(
            document,
            "$.windows[0].access_points[0].phy_stats.uplink.average_data_rate_mbps",
        )

    def test_rejects_invalid_nested_and_configuration_types(self):
        document = _valid_document(self.trace)
        agent = document["windows"][0]["access_points"][0]["app_stats"]["uplink"]["agents"][0]
        agent["agent_key"] = 1
        self.assert_document_error(
            document,
            "$.windows[0].access_points[0].app_stats.uplink.agents[0].agent_key",
        )

        document = _valid_document(self.trace)
        document["experiment_metadata"]["configuration"]["simulation"]["rng_seed"] = True
        self.assert_document_error(
            document,
            "$.experiment_metadata.configuration.simulation.rng_seed",
        )

        document = _valid_document(self.trace)
        document["experiment_metadata"]["configuration"]["general"]["trace_file"] = None
        self.assert_document_error(
            document,
            "$.experiment_metadata.configuration.general.trace_file",
        )

    def test_rejects_malformed_json_with_path(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "output.json"
            output.write_text("{broken", encoding="utf-8")
            with self.assertRaisesRegex(LiveTraceError, str(output)):
                load_output_document(output, self.trace)

    def test_rejects_malformed_hierarchy_with_path(self):
        self.assert_path_error(
            validate_output_document, [], self.source, self.trace, text="expected object"
        )

    def test_rejects_removed_root(self):
        document = _valid_document(self.trace)
        document["wifi_windows"] = []
        self.assert_path_error(
            validate_output_document, document, self.source, self.trace, text="wifi_windows"
        )

    def test_rejects_bad_entity(self):
        document = _valid_document(self.trace)
        document["windows"][0]["stations"][0]["node_id"] = 999
        self.assert_path_error(
            validate_output_document, document, self.source, self.trace, text="inventory"
        )

    def test_rejects_missing_category(self):
        document = _valid_document(self.trace)
        del document["overall"]["access_points"][0]["tcp_stats"]
        self.assert_path_error(
            validate_output_document, document, self.source, self.trace, text="tcp_stats"
        )

    def test_rejects_missing_validation_flag(self):
        document = _valid_document(self.trace)
        del document["validation"]["overall_matches_windows"]
        self.assert_path_error(
            validate_output_document,
            document,
            self.source,
            self.trace,
            text="overall_matches_windows",
        )

    def test_rejects_legacy_console_marker(self):
        with self.assertRaisesRegex(LiveTraceError, "Final per-second"):
            reject_legacy_console("prefix [Final per-second] row", self.source)

    def test_owned_temporary_run_context_cleans_normal_directory(self):
        with tempfile.TemporaryDirectory() as directory:
            temporary_parent = Path(directory)
            with OwnedTemporaryRun.create("test", temporary_parent) as owner:
                owned_path = owner.path
                (owned_path / "output.json").write_text("{}", encoding="utf-8")
                self.assertTrue(owned_path.is_dir())
            self.assertFalse(owned_path.exists())

    def test_owned_cleanup_is_idempotent(self):
        with tempfile.TemporaryDirectory() as directory:
            owner = OwnedTemporaryRun.create("test", Path(directory))
            cleanup_run_directory(owner)
            cleanup_run_directory(owner)

    def test_cleanup_uses_private_dirfd_quarantine(self):
        with tempfile.TemporaryDirectory() as directory:
            owner = OwnedTemporaryRun.create("test", Path(directory))
            original_rename = os.rename
            calls = []

            def record_rename(source, destination, **kwargs):
                calls.append((source, destination, kwargs))
                return original_rename(source, destination, **kwargs)

            with mock.patch.object(os, "rename", side_effect=record_rename):
                cleanup_run_directory(owner)

            self.assertGreaterEqual(len(calls), 1)
            source, destination, kwargs = calls[0]
            self.assertEqual(source, owner.path.name)
            self.assertRegex(destination, r"^\.llm-trace-live-quarantine\.[0-9a-f]{32}$")
            self.assertIsInstance(kwargs.get("src_dir_fd"), int)
            self.assertEqual(kwargs.get("src_dir_fd"), kwargs.get("dst_dir_fd"))

    def test_cleanup_refuses_substitution_immediately_before_atomic_rename(self):
        with tempfile.TemporaryDirectory() as directory:
            temporary_parent = Path(directory)
            owner = OwnedTemporaryRun.create("test", temporary_parent)
            original_rename = os.rename
            parked = temporary_parent / "parked-original"
            replacement_marker = owner.path / "replacement-marker"
            race_injected = False

            def substitute_then_rename(source, destination, **kwargs):
                nonlocal race_injected
                if not race_injected:
                    race_injected = True
                    original_rename(owner.path, parked)
                    owner.path.mkdir()
                    replacement_marker.write_text("replacement", encoding="utf-8")
                return original_rename(source, destination, **kwargs)

            with mock.patch.object(os, "rename", side_effect=substitute_then_rename):
                with self.assertRaises(LiveTraceError):
                    cleanup_run_directory(owner)

            self.assertTrue(race_injected)
            self.assertEqual(replacement_marker.read_text(encoding="utf-8"), "replacement")
            self.assertTrue(parked.is_dir())

    def test_cleanup_never_follows_owned_symlink_outside_directory(self):
        with tempfile.TemporaryDirectory() as directory:
            temporary_parent = Path(directory)
            owner = OwnedTemporaryRun.create("test", temporary_parent)
            outside = temporary_parent / "outside"
            outside.mkdir()
            marker = outside / "preserve"
            marker.write_text("outside", encoding="utf-8")
            (owner.path / "outside-link").symlink_to(outside, target_is_directory=True)
            nested = owner.path / "nested"
            nested.mkdir()
            (nested / "owned").write_text("delete", encoding="utf-8")

            cleanup_run_directory(owner)

            self.assertEqual(marker.read_text(encoding="utf-8"), "outside")

    def test_cleanup_refuses_matching_name_without_owner(self):
        with tempfile.TemporaryDirectory() as directory:
            unowned = Path(directory) / "llm-trace-live.test.random"
            unowned.mkdir()
            marker = unowned / "preserve"
            marker.write_text("user-owned", encoding="utf-8")
            with self.assertRaises(LiveTraceError):
                cleanup_run_directory(unowned)
            self.assertEqual(marker.read_text(encoding="utf-8"), "user-owned")

    def test_cleanup_refuses_symlink_substitution(self):
        with tempfile.TemporaryDirectory() as directory:
            temporary_parent = Path(directory)
            owner = OwnedTemporaryRun.create("test", temporary_parent)
            parked = temporary_parent / "parked-original"
            owner.path.rename(parked)
            victim = temporary_parent / "victim"
            victim.mkdir()
            marker = victim / "preserve"
            marker.write_text("user-owned", encoding="utf-8")
            owner.path.symlink_to(victim, target_is_directory=True)

            with self.assertRaises(LiveTraceError):
                cleanup_run_directory(owner)
            self.assertTrue(owner.path.is_symlink())
            self.assertEqual(marker.read_text(encoding="utf-8"), "user-owned")
            self.assertTrue(parked.is_dir())

    def test_cleanup_refuses_inode_replacement(self):
        with tempfile.TemporaryDirectory() as directory:
            temporary_parent = Path(directory)
            owner = OwnedTemporaryRun.create("test", temporary_parent)
            parked = temporary_parent / "parked-original"
            owner.path.rename(parked)
            owner.path.mkdir()
            marker = owner.path / "preserve"
            marker.write_text("replacement", encoding="utf-8")

            with self.assertRaises(LiveTraceError):
                cleanup_run_directory(owner)
            self.assertEqual(marker.read_text(encoding="utf-8"), "replacement")
            self.assertTrue(parked.is_dir())

    def test_cleans_up_after_command_failure(self):
        self._assert_run_failure_cleans(parse_failure=False)

    def test_cleans_up_after_parse_failure(self):
        self._assert_run_failure_cleans(parse_failure=True)

    def test_cleans_up_after_command_construction_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            outer = Path(directory)
            trace = outer / self.trace
            trace.parent.mkdir(parents=True)
            trace.write_text('{"traces": []}', encoding="utf-8")
            temporary_parent = outer / "tmp"
            temporary_parent.mkdir()

            def fake_run(command, **kwargs):
                return subprocess.CompletedProcess(command, 0, stdout="valid\n")

            invalid_policy = {"mode": "invalid", "timeout_seconds": 1}
            with self.assertRaises(LiveTraceError):
                run_one_trace(
                    outer,
                    trace,
                    invalid_policy,
                    run_process=fake_run,
                    temporary_parent=temporary_parent,
                )
            self.assertEqual(list(temporary_parent.iterdir()), [])

    def _assert_run_failure_cleans(self, parse_failure):
        with tempfile.TemporaryDirectory() as directory:
            outer = Path(directory)
            trace = outer / self.trace
            trace.parent.mkdir(parents=True)
            trace.write_text('{"traces": []}', encoding="utf-8")
            temporary_parent = outer / "tmp"
            temporary_parent.mkdir()

            def fake_run(command, **kwargs):
                self.assertFalse(kwargs["check"])
                self.assertEqual(kwargs["timeout"], POLICY[trace.name]["timeout_seconds"])
                if command[0] == "python3":
                    return subprocess.CompletedProcess(command, 0, stdout="valid\n")
                run_arguments = shlex.split(command[2])
                run_directory = Path(
                    run_arguments[run_arguments.index("--general-run-folder") + 1]
                )
                if parse_failure:
                    (run_directory / "output.json").write_text("{broken", encoding="utf-8")
                    return subprocess.CompletedProcess(command, 0, stdout="completed\n")
                return subprocess.CompletedProcess(command, 7, stdout="failed\n")

            with self.assertRaises(LiveTraceError):
                run_one_trace(
                    outer,
                    trace,
                    POLICY[trace.name],
                    run_process=fake_run,
                    temporary_parent=temporary_parent,
                )
            self.assertEqual(list(temporary_parent.iterdir()), [])


def run_self_tests():
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(LiveTraceSelfTest)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return 0 if result.wasSuccessful() else 1


def run_live_matrix():
    outer_root = Path(__file__).resolve().parents[3]
    trace_directory = outer_root / "contrib/llm/traces"
    discovered = sorted(trace_directory.glob("*.json"))
    names = validate_policy_coverage(discovered, trace_directory)
    failures = []
    for name in names:
        trace_path = trace_directory / name
        try:
            metrics = run_one_trace(outer_root, trace_path, POLICY[name])
            validation = ",".join(
                f"{key}={str(value).lower()}" for key, value in sorted(metrics["validation"].items())
            )
            print(
                f"PASS trace={name} policy={metrics['policy']} "
                f"timeout_seconds={POLICY[name]['timeout_seconds']} "
                f"return_code={metrics['return_code']} wall_time_s={metrics['wall_time_s']:.3f} "
                f"output_bytes={metrics['output_bytes']} window_count={metrics['window_count']} "
                f"ap_inventory_count={metrics['ap_inventory_count']} "
                f"sta_inventory_count={metrics['sta_inventory_count']} validation={validation}",
                flush=True,
            )
        except LiveTraceError as error:
            failures.append(str(error))
            print(str(error), file=sys.stderr, flush=True)
    if failures:
        raise LiveTraceError(f"live matrix failed for {len(failures)} trace(s)")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true", help="run deterministic script tests")
    arguments = parser.parse_args()
    try:
        return run_self_tests() if arguments.self_test else run_live_matrix()
    except LiveTraceError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
