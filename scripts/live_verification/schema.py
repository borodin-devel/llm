"""Root, configuration, window, inventory, and entity identity validation."""

from __future__ import annotations

import json
from pathlib import Path

from live_verification.common import (
    AP_IDENTITY_KEYS, CATEGORY_KEYS, CONFIGURATION_KEYS, MEASUREMENT_SEMANTICS,
    ROOT_KEYS, STA_IDENTITY_KEYS, VALIDATION_KEYS, WINDOW_KEYS, LiveTraceError,
    expect_boolean, expect_finite_number, expect_list, expect_nonnegative_integer,
    expect_object_keys, expect_string, fail, is_nonnegative_integer, reject_removed_keys,
    validate_ordered_unique,
)
from live_verification.schema_categories import validate_entity_categories


def _validate_configuration(configuration, source_path):
    base = "$.experiment_metadata.configuration"
    general = configuration["general"]
    expect_string(general["trace_file"], source_path, f"{base}.general.trace_file")
    expect_string(
        general["run_folder"], source_path, f"{base}.general.run_folder", allow_none=True
    )
    expect_string(general["output_name"], source_path, f"{base}.general.output_name")

    simulation = configuration["simulation"]
    expect_string(
        simulation["duration_mode"], source_path, f"{base}.simulation.duration_mode",
        allowed={"auto", "fixed"},
    )
    expect_finite_number(
        simulation["fixed_duration_seconds"], source_path,
        f"{base}.simulation.fixed_duration_seconds",
    )
    if simulation["duration_mode"] == "fixed" and simulation["fixed_duration_seconds"] <= 0:
        fail(
            source_path, f"{base}.simulation.fixed_duration_seconds",
            "expected positive value in fixed mode",
        )
    expect_finite_number(
        simulation["auto_tail_seconds"], source_path, f"{base}.simulation.auto_tail_seconds"
    )
    expect_nonnegative_integer(
        simulation["rng_seed"], source_path, f"{base}.simulation.rng_seed",
        positive=True, maximum=4294944442,
    )
    expect_nonnegative_integer(
        simulation["rng_run"], source_path, f"{base}.simulation.rng_run"
    )

    topology = configuration["topology"]
    expect_nonnegative_integer(
        topology["bss_count"], source_path, f"{base}.topology.bss_count",
        positive=True, maximum=256,
    )
    expect_nonnegative_integer(
        topology["stations_per_bss"], source_path, f"{base}.topology.stations_per_bss",
        positive=True, maximum=253,
    )
    for field in ("bss_spacing_m", "station_radius_m", "generator_start_seconds"):
        expect_finite_number(topology[field], source_path, f"{base}.topology.{field}")
    expect_boolean(
        topology["isolate_bss_channels"], source_path,
        f"{base}.topology.isolate_bss_channels",
    )
    expect_string(topology["ssid_prefix"], source_path, f"{base}.topology.ssid_prefix")
    for field in ("ap_sink_port", "station_sink_base_port"):
        expect_nonnegative_integer(
            topology[field], source_path, f"{base}.topology.{field}",
            positive=True, maximum=65535,
        )

    distribution = configuration["distribution"]
    expect_nonnegative_integer(
        distribution["max_agents_per_station"], source_path,
        f"{base}.distribution.max_agents_per_station",
    )
    expect_boolean(
        distribution["low_contention_priority"], source_path,
        f"{base}.distribution.low_contention_priority",
    )
    expect_nonnegative_integer(
        distribution["slot_ms"], source_path, f"{base}.distribution.slot_ms", positive=True
    )

    wifi = configuration["wifi"]
    expect_string(
        wifi["band"], source_path, f"{base}.wifi.band", allowed={"2.4GHz", "5GHz", "6GHz"}
    )
    expect_nonnegative_integer(
        wifi["channel_number"], source_path, f"{base}.wifi.channel_number", maximum=65535
    )
    expect_nonnegative_integer(
        wifi["bandwidth_mhz"], source_path, f"{base}.wifi.bandwidth_mhz", positive=True
    )
    if wifi["bandwidth_mhz"] not in {20, 40, 80, 160}:
        fail(source_path, f"{base}.wifi.bandwidth_mhz", "expected 20, 40, 80, or 160")
    expect_nonnegative_integer(
        wifi["primary_20_index"], source_path, f"{base}.wifi.primary_20_index", maximum=255
    )
    if wifi["primary_20_index"] >= wifi["bandwidth_mhz"] // 20:
        fail(source_path, f"{base}.wifi.primary_20_index", "outside configured bandwidth")
    expect_string(wifi["rate_manager"], source_path, f"{base}.wifi.rate_manager")
    expect_boolean(wifi["active_probing"], source_path, f"{base}.wifi.active_probing")

    tcp = configuration["tcp"]
    expect_string(tcp["congestion_control"], source_path, f"{base}.tcp.congestion_control")
    for field in ("segment_size_bytes", "send_buffer_bytes", "receive_buffer_bytes"):
        expect_nonnegative_integer(
            tcp[field], source_path, f"{base}.tcp.{field}", positive=True
        )
    expect_nonnegative_integer(
        configuration["statistics"]["window_ms"], source_path,
        f"{base}.statistics.window_ms", positive=True,
    )
    log_levels = {"off", "error", "warn", "info", "debug", "function", "logic", "all"}
    for field, value in configuration["logging"].items():
        expect_string(value, source_path, f"{base}.logging.{field}", allowed=log_levels)


def _identity_key(record, kind):
    if kind == "access_point":
        return record["access_point_id"]
    return record["access_point_id"], record["station_index"]


def _identity_label(kind):
    return "access_point_id" if kind == "access_point" else "station identity"


def _validate_identity(record, kind, source_path, json_path, include_categories=False):
    identity_keys = AP_IDENTITY_KEYS if kind == "access_point" else STA_IDENTITY_KEYS
    expected_keys = identity_keys | CATEGORY_KEYS if include_categories else identity_keys
    expect_object_keys(record, expected_keys, source_path, json_path)
    for key in identity_keys & {"access_point_id", "station_index", "node_id"}:
        expect_nonnegative_integer(record[key], source_path, f"{json_path}.{key}")
    for key in {"node_label", "ipv4"}:
        expect_string(record[key], source_path, f"{json_path}.{key}")
    return _identity_key(record, kind)


def _build_inventory(records, kind, source_path, json_path):
    expect_list(records, source_path, json_path)
    inventory = {}
    for index, record in enumerate(records):
        record_path = f"{json_path}[{index}]"
        identity = _validate_identity(record, kind, source_path, record_path)
        if identity in inventory:
            fail(
                source_path, record_path,
                f"duplicate {_identity_label(kind)} {identity!r}",
            )
        inventory[identity] = record
    validate_ordered_unique(
        records, lambda record: _identity_key(record, kind), source_path, json_path,
        _identity_label(kind),
    )
    return inventory


def _build_global_node_map(ap_records, sta_records, source_path):
    known_nodes = {}
    known_ips = {}
    groups = (
        ("access_points", ap_records),
        ("stations", sta_records),
    )
    base = "$.experiment_metadata.entity_inventory"
    for group_name, records in groups:
        for index, record in enumerate(records):
            record_path = f"{base}.{group_name}[{index}]"
            node_id = record["node_id"]
            if node_id in known_nodes:
                fail(source_path, f"{record_path}.node_id", f"duplicate node_id {node_id}")
            ipv4 = record["ipv4"]
            if ipv4 in known_ips:
                fail(source_path, f"{record_path}.ipv4", f"duplicate ipv4 {ipv4!r}")
            known_nodes[node_id] = record
            known_ips[ipv4] = record
    return known_nodes


def _validate_entity(record, kind, inventory, known_nodes, source_path, json_path):
    identity = _validate_identity(
        record, kind, source_path, json_path, include_categories=True
    )
    if identity not in inventory:
        fail(source_path, json_path, f"entity identity {identity!r} does not reference inventory")
    inventory_identity = inventory[identity]
    keys = AP_IDENTITY_KEYS | ({"station_index"} if kind == "station" else set())
    for key in keys:
        if record[key] != inventory_identity[key]:
            fail(source_path, f"{json_path}.{key}", "does not match inventory")
    validate_entity_categories(record, known_nodes, source_path, json_path)
    return identity


def _validate_entity_array(records, kind, inventory, known_nodes, source_path, json_path):
    expect_list(records, source_path, json_path)
    identities = []
    for index, record in enumerate(records):
        identities.append(
            _validate_entity(
                record, kind, inventory, known_nodes, source_path, f"{json_path}[{index}]"
            )
        )
    validate_ordered_unique(
        records, lambda record: _identity_key(record, kind), source_path, json_path,
        _identity_label(kind),
    )
    return records


def validate_output_document(
    document,
    source_path,
    expected_trace,
    expected_run_directory=None,
    expected_policy=None,
):
    """Validate one parsed output document and return its live metrics."""
    source_path = Path(source_path)
    reject_removed_keys(document, source_path, "$")
    expect_object_keys(document, ROOT_KEYS, source_path, "$")
    if type(document["schema_version"]) is not int or document["schema_version"] != 1:
        fail(source_path, "$.schema_version", "expected integer 1")
    semantics = document["measurement_semantics"]
    expect_object_keys(semantics, set(MEASUREMENT_SEMANTICS), source_path, "$.measurement_semantics")
    for key, expected_value in MEASUREMENT_SEMANTICS.items():
        if semantics[key] != expected_value or (
            expected_value is not None and type(semantics[key]) is not str
        ):
            fail(source_path, f"$.measurement_semantics.{key}", f"expected {expected_value!r}")
    window_width = document["statistics_window_ms"]
    expect_nonnegative_integer(
        window_width, source_path, "$.statistics_window_ms", positive=True
    )

    metadata = document["experiment_metadata"]
    expect_object_keys(
        metadata, {"configuration", "entity_inventory"}, source_path, "$.experiment_metadata"
    )
    configuration = metadata["configuration"]
    expect_object_keys(
        configuration, set(CONFIGURATION_KEYS), source_path,
        "$.experiment_metadata.configuration",
    )
    field_count = 0
    for section, expected_fields in CONFIGURATION_KEYS.items():
        section_path = f"$.experiment_metadata.configuration.{section}"
        expect_object_keys(configuration[section], expected_fields, source_path, section_path)
        field_count += len(configuration[section])
    if len(configuration) != 8 or field_count != 36:
        fail(
            source_path, "$.experiment_metadata.configuration",
            f"expected 8 sections and 36 fields, got {len(configuration)} and {field_count}",
        )
    _validate_configuration(configuration, source_path)
    general = configuration["general"]
    if general["trace_file"] != str(expected_trace):
        fail(
            source_path, "$.experiment_metadata.configuration.general.trace_file",
            f"expected {str(expected_trace)!r}",
        )
    if general["output_name"] != "output.json":
        fail(
            source_path, "$.experiment_metadata.configuration.general.output_name",
            "expected default output.json",
        )
    if expected_run_directory is not None and general["run_folder"] != str(expected_run_directory):
        fail(
            source_path, "$.experiment_metadata.configuration.general.run_folder",
            f"expected {str(expected_run_directory)!r}",
        )
    if configuration["statistics"]["window_ms"] != window_width:
        fail(
            source_path, "$.experiment_metadata.configuration.statistics.window_ms",
            "does not match statistics_window_ms",
        )
    if expected_policy is not None:
        expected_mode = expected_policy["mode"]
        simulation = configuration["simulation"]
        if simulation["duration_mode"] != expected_mode:
            fail(
                source_path, "$.experiment_metadata.configuration.simulation.duration_mode",
                f"expected {expected_mode!r}",
            )
        if (
            expected_mode == "fixed"
            and simulation["fixed_duration_seconds"] != expected_policy["seconds"]
        ):
            fail(
                source_path,
                "$.experiment_metadata.configuration.simulation.fixed_duration_seconds",
                f"expected {expected_policy['seconds']!r}",
            )

    inventory_root = metadata["entity_inventory"]
    expect_object_keys(
        inventory_root, {"access_points", "stations"}, source_path,
        "$.experiment_metadata.entity_inventory",
    )
    ap_records = inventory_root["access_points"]
    sta_records = inventory_root["stations"]
    ap_inventory = _build_inventory(
        ap_records, "access_point", source_path,
        "$.experiment_metadata.entity_inventory.access_points",
    )
    sta_inventory = _build_inventory(
        sta_records, "station", source_path,
        "$.experiment_metadata.entity_inventory.stations",
    )
    if not ap_inventory:
        fail(
            source_path, "$.experiment_metadata.entity_inventory.access_points",
            "expected at least one access point",
        )
    if not sta_inventory:
        fail(
            source_path, "$.experiment_metadata.entity_inventory.stations",
            "expected at least one station",
        )
    known_nodes = _build_global_node_map(ap_records, sta_records, source_path)
    for identity, record in sta_inventory.items():
        if record["access_point_id"] not in ap_inventory:
            fail(
                source_path, "$.experiment_metadata.entity_inventory.stations",
                f"station {identity!r} references unknown access_point_id",
            )

    windows = document["windows"]
    expect_list(windows, source_path, "$.windows")
    if not windows:
        fail(source_path, "$.windows", "expected at least one sparse window")
    prior_index = -1
    for position, window in enumerate(windows):
        window_path = f"$.windows[{position}]"
        expect_object_keys(window, WINDOW_KEYS, source_path, window_path)
        index = window["window_index"]
        if not is_nonnegative_integer(index) or index <= prior_index:
            fail(source_path, f"{window_path}.window_index", "expected increasing index")
        prior_index = index
        start = window["window_start_ms"]
        duration = window["window_duration_ms"]
        expect_finite_number(start, source_path, f"{window_path}.window_start_ms")
        expect_finite_number(
            duration, source_path, f"{window_path}.window_duration_ms", positive=True
        )
        if abs(start - index * window_width) > 1e-6:
            fail(source_path, f"{window_path}.window_start_ms", "does not match index * width")
        if duration > window_width + 1e-6:
            fail(
                source_path, f"{window_path}.window_duration_ms",
                "expected a positive full or partial configured window",
            )
        if position + 1 < len(windows) and abs(duration - window_width) > 1e-6:
            fail(
                source_path, f"{window_path}.window_duration_ms",
                "only the last emitted window may be partial",
            )
        ap_entities = _validate_entity_array(
            window["access_points"], "access_point", ap_inventory, known_nodes,
            source_path, f"{window_path}.access_points",
        )
        sta_entities = _validate_entity_array(
            window["stations"], "station", sta_inventory, known_nodes,
            source_path, f"{window_path}.stations",
        )
        if not ap_entities:
            fail(source_path, f"{window_path}.access_points", "missing active BSS parent")
        emitted_ap_ids = {entity["access_point_id"] for entity in ap_entities}
        for entity in sta_entities:
            if entity["access_point_id"] not in emitted_ap_ids:
                fail(
                    source_path, f"{window_path}.stations",
                    f"station node {entity['node_id']} lacks its AP BSS parent",
                )

    overall = document["overall"]
    expect_object_keys(overall, {"access_points", "stations"}, source_path, "$.overall")
    overall_aps = _validate_entity_array(
        overall["access_points"], "access_point", ap_inventory, known_nodes,
        source_path, "$.overall.access_points",
    )
    overall_stas = _validate_entity_array(
        overall["stations"], "station", sta_inventory, known_nodes,
        source_path, "$.overall.stations",
    )
    if {_identity_key(entity, "access_point") for entity in overall_aps} != set(ap_inventory):
        fail(source_path, "$.overall.access_points", "is not dense over AP inventory")
    if {_identity_key(entity, "station") for entity in overall_stas} != set(sta_inventory):
        fail(source_path, "$.overall.stations", "is not dense over STA inventory")

    validation = document["validation"]
    expect_object_keys(validation, VALIDATION_KEYS, source_path, "$.validation")
    for key in sorted(VALIDATION_KEYS):
        if type(validation[key]) is not bool or not validation[key]:
            fail(source_path, f"$.validation.{key}", "expected true Boolean")

    return {
        "window_count": len(windows),
        "ap_inventory_count": len(ap_inventory),
        "sta_inventory_count": len(sta_inventory),
        "validation": dict(validation),
    }


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
