# Experiment JSON Output Design

## Status

Approved in conversation on 2026-08-25. This specification intentionally
defines a breaking JSON schema. Backward compatibility with the existing field
names is not required; clarity and explicit measurement units take priority.

## Goal

Write every end-of-experiment measurement to one structured JSON document,
including the information currently emitted by
`TrafficFlowMonitor::PrintTransmissionTimePerSender()` and
`WifiStatistics::PrintCrossLayerReport()`. The document ends with all 36
effective scenario configuration fields. Summary measurements must no longer
be printed to stdout or ns-3 logs.

The default JSON filename changes from `mac-node-stats.json` to `output.json`.

## Non-Goals

- Preserve old JSON field names or the old root structure.
- Record CLI/TOML provenance for each effective value.
- Record resolved config, trace, run-folder, or output paths.
- Build the complete output as an in-memory JSON DOM.
- Change the simulation, traffic generation, collection points, or metric
  formulas except where the current report uses an imprecise unit conversion.
- Remove ordinary startup, progress, error, or completion messages from stdout
  and logs. Only end-of-experiment measurement summaries move to JSON.

## Current State

`WifiStatistics::WriteJson()` exclusively creates the output file and streams
sparse Wi-Fi windows, a per-AP summary, and validation flags. Main calls that
writer after `Simulator::Run()`, then separately calls:

```cpp
trafficFlowMonitor.PrintTransmissionTimePerSender();
wifiStatistics.PrintCrossLayerReport();
```

The traffic-flow report aggregates matched transmit/receive durations and
transmitted payload bytes by source IPv4 address. The cross-layer report emits
per-node, per-second measurements plus per-node overall totals, MAC MPDU drop
reasons, and per-agent application drops. Both reports currently use ns-3 log
macros and cannot be consumed reliably by tools.

The effective scenario is represented by `ScenarioConfig`. A single private
registry owns all 36 dotted TOML keys, CLI names, types, setters, and help
descriptions. That registry is the authoritative configuration field list.

## Selected Architecture

Use typed summary records plus one streaming experiment serializer.

1. `TrafficFlowMonitor::BuildTransmissionSummary()` returns typed per-sender
   records without printing or logging.
2. `WifiStatistics::BuildCrossLayerSummary()` returns typed per-node interval
   and overall records without printing or logging.
3. `WifiStatistics::WriteExperimentJson()` receives both summaries and the
   effective `ScenarioConfig`, exclusively creates the configured output, and
   streams the complete document.
4. Existing Wi-Fi-window serialization remains streaming and bounded-memory.
5. The configuration registry gains a scalar serialization callback so the
   output metadata cannot drift from TOML/CLI mappings.
6. `nlohmann::json` is used only to encode individual scalar values and escape
   strings. Large arrays are not duplicated in a DOM.

This keeps measurement aggregation independently testable and avoids coupling
each statistics subsystem to root JSON punctuation and field ordering.

## File Responsibilities

### `experiment-output.h`

New focused header containing the typed output records shared by the monitor,
statistics owner, serializer, and tests:

- `TransmissionSenderSummary` and `TransmissionSummary`;
- `DelaySummary`;
- `MacDropReasonSummary`;
- `AgentDropSummary`;
- `CrossLayerIntervalSummary`;
- `CrossLayerOverallSummary`;
- `CrossLayerNodeSummary` and `CrossLayerSummary`.

Every public class, member, parameter, and return value follows ns-3 Doxygen
rules. Names include units where applicable.

### `traffic-flow-monitor.h/.cc`

Replace `PrintTransmissionTimePerSender()` with:

```cpp
TransmissionSummary BuildTransmissionSummary() const;
```

The existing private trace state remains private. Aggregation uses `uint64_t`
from the first accumulation step; it must not use an `int` initial value.

### `wifi-statistics-summary.cc`

Replace the log-oriented `wifi-statistics-report.cc` implementation with a
pure cross-layer summary builder. The public method is:

```cpp
CrossLayerSummary BuildCrossLayerSummary() const;
```

It iterates every registered node and every configured one-second interval,
including all-zero rows, matching the current report behavior. It builds
per-interval drop-reason and per-agent-drop arrays and per-node overall totals.

### `wifi-statistics-json.cc`

Retain focused streaming of:

- `statistics_window_ms`;
- `wifi_windows`;
- `wifi_summary`;
- `validation`.

It no longer owns the complete root document or output file. Internal helpers
write these members to a provided stream and return validation information
needed by the root serializer.

### `experiment-json.cc`

New root serializer and exclusive file owner. It writes root members in the
specified order, delegates Wi-Fi sections, writes both typed summaries, and
serializes configuration metadata from the registry. It preserves the current
C++23 `std::ios::noreplace` no-clobber behavior and explicit write, flush, and
close error propagation.

The public owner method is:

```cpp
void WifiStatistics::WriteExperimentJson(
    const std::string& outputPath,
    const TransmissionSummary& transmissionSummary,
    const CrossLayerSummary& crossLayerSummary,
    const ScenarioConfig& configuration) const;
```

### `scenario-config-internal.h` and `scenario-config.cc`

`ConfigOption` gains a callback that reads its effective value from a const
`ScenarioConfig` and returns a scalar `nlohmann::json` value. Every existing
factory constructs the callback from the same accessor and enum mapping used
by TOML and CLI setters:

- strings remain JSON strings;
- optional `run_folder` becomes a string or `null`;
- integers retain integer types;
- finite floating-point values retain numeric types;
- booleans retain Boolean types;
- enums use their canonical TOML spelling.

No second 36-field switch or list is permitted.

### `sample-scenario.cc`

After `Simulator::Run()` and the barrier assertion, main performs:

```cpp
const TransmissionSummary transmissionSummary =
    trafficFlowMonitor.BuildTransmissionSummary();
const CrossLayerSummary crossLayerSummary = wifiStatistics.BuildCrossLayerSummary();
wifiStatistics.WriteExperimentJson(resolvedPaths.outputFile.string(),
                                   transmissionSummary,
                                   crossLayerSummary,
                                   config);
```

Summary-building and writing share the existing error path: destroy simulator
state, print a concise error, and return 1. Main no longer calls either print
method. Ordinary runtime and completion messages remain.

## JSON Schema

The root schema version is integer `1`. Properties are emitted in this order:

```json
{
  "schema_version": 1,
  "measurement_semantics": {},
  "statistics_window_ms": 10,
  "wifi_windows": [],
  "wifi_summary": [],
  "transmission_summary": {"senders": []},
  "cross_layer_summary": {"nodes": []},
  "validation": {},
  "experiment_metadata": {"configuration": {}}
}
```

JSON object order is not semantically significant, but the streaming writer
keeps `experiment_metadata` physically last as requested.

### Naming Rules

- Use complete direction names: `uplink` and `downlink`.
- Measurement names end with a unit suffix where applicable: `_us`, `_ms`,
  `_s`, `_bytes`, `_mbps`, or `_percent`.
- Event/sample/attempt/drop/retransmission quantities end with `_count`.
- IDs, indexes, enum strings, Boolean states, paths, and names are unitless.
- IPv4 address fields end with `_ipv4`.
- A missing numeric measurement is JSON `null`, not a magic negative value.

### Measurement Semantics

`measurement_semantics` retains the existing three explanatory strings under
clearer names:

```json
{
  "mac_payload_source": "PhyTxBegin+PhyTxPsduBegin/AppTxTag",
  "mac_payload_byte_semantics": "...",
  "phy_data_rate_semantics": "..."
}
```

The existing descriptions remain substantively unchanged: tagged application
payload observed at PHY includes retransmission attempts, and PHY data rate is
the airtime-weighted nominal `WifiTxVector` rate of tagged PPDU attempts.

### Wi-Fi Windows

`wifi_windows` remains sparse. Each item represents one non-empty statistics
window and uses the window end time:

```json
{
  "window_end_ms": 25,
  "access_points": [
    {
      "access_point_id": 0,
      "uplink": {
        "total_payload_bytes": 1000,
        "flows": [
          {
            "station_ipv4": "10.1.0.2",
            "payload_bytes": 1000,
            "throughput_mbps": 0.32,
            "average_phy_data_rate_mbps": 86.0,
            "phy_transmission_attempt_count": 1,
            "phy_transmission_airtime_us": 92.5
          }
        ]
      },
      "downlink": {
        "total_payload_bytes": 0,
        "flows": []
      }
    }
  ]
}
```

The throughput formula remains:

```text
throughput_mbps = payload_bytes * 8 / statistics_window_us
```

`average_phy_data_rate_mbps` is `null` when no PHY attempt exists.

The following old ambiguous names are removed:

```text
timestamp, stats, ap_id, up_flows, down_flows,
up_total_bytes, down_total_bytes, host_id, bytes, bw,
avg_phy_data_rate_mbps, phy_tx_attempts, phy_tx_airtime_us
```

### Wi-Fi Summary

`wifi_summary` contains one object per registered access point with the same
`access_point_id`, `uplink`, and `downlink` shape. Each direction contains
`total_payload_bytes` and `flows`. A summary flow contains:

```text
station_ipv4
total_payload_bytes
average_phy_data_rate_mbps
phy_transmission_attempt_count
phy_transmission_airtime_us
```

It does not report a throughput over the whole experiment because the existing
summary does not define that measurement.

### Transmission Summary

`transmission_summary.senders` contains every sender with a recorded transmit
payload sample, ordered by IPv4 string as in the existing `std::map` state:

```json
{
  "sender_ipv4": "10.1.0.2",
  "matched_packet_count": 10,
  "total_transmission_duration_us": 2400,
  "transmitted_payload_bytes": 12000,
  "effective_throughput_mbps": 40.0
}
```

`matched_packet_count` counts transmit/receive pairs with a strictly positive
duration. `total_transmission_duration_us` is the sum of those durations.
`transmitted_payload_bytes` preserves the existing MAC transmit sample total,
including repeated attempts observed by that trace source.

The effective rate uses SI Mbps directly:

```text
effective_throughput_mbps = transmitted_payload_bytes * 8 /
                            total_transmission_duration_us
```

Bytes divided by microseconds after multiplying by eight is Mbps. When no
positive matched duration exists, count and duration are zero and
`effective_throughput_mbps` is `null`.

### Cross-Layer Summary

`cross_layer_summary.nodes` contains every registered node, including nodes
with no measurements. Each node contains:

```text
node_id
node_label
one_second_intervals
overall
```

Each `one_second_intervals` entry contains:

```text
interval_index
interval_start_s
interval_duration_s
application_to_phy_delay
application_transmit_throughput_mbps
phy_payload_throughput_mbps
unique_phy_payload_throughput_mbps
channel_utilization_percent
phy_retransmission_count
mac_transmit_drop_count
mac_transmit_drop_bytes
mac_mpdu_drop_count
mac_mpdu_drop_bytes
mac_data_failure_count
mac_final_data_failure_count
application_drop_event_count
application_drop_bytes
mac_mpdu_drops_by_reason
application_drops_by_agent
```

The last interval may have `interval_duration_s < 1.0`. Throughput and channel
utilization use that actual duration, matching the current report.

`application_to_phy_delay` contains:

```text
sample_count
mean_us
standard_deviation_us
minimum_us
maximum_us
```

All four delay values are `0.0` when `sample_count` is zero.

Drop breakdown entries use:

```json
{"reason_code": 7, "drop_count": 3}
```

and:

```json
{"agent_key": "agent-1", "drop_event_count": 2, "dropped_payload_bytes": 4096}
```

Each node's `overall` object contains:

```text
experiment_duration_s
application_to_phy_delay
application_transmitted_payload_bytes
phy_payload_bytes
unique_phy_payload_bytes
phy_mpdu_bytes
average_application_transmit_throughput_mbps
average_phy_payload_throughput_mbps
average_channel_utilization_percent
phy_retransmission_count
mac_transmit_drop_count
mac_transmit_drop_bytes
mac_mpdu_drop_count
mac_mpdu_drop_bytes
mac_data_failure_count
mac_final_data_failure_count
application_drop_event_count
application_drop_bytes
mac_mpdu_drops_by_reason
```

These are the measurements currently emitted by the overall report, renamed
only for precision and units.

### Validation

`validation` contains:

```json
{
  "window_payload_totals_consistent": true,
  "summary_payload_totals_consistent": true
}
```

These replace `window_totals_consistent` and `summary_totals_consistent` to
state what is being compared.

### Experiment Metadata

The final root member is:

```json
{
  "experiment_metadata": {
    "configuration": {
      "general": {},
      "simulation": {},
      "topology": {},
      "distribution": {},
      "wifi": {},
      "tcp": {},
      "statistics": {},
      "logging": {}
    }
  }
}
```

It contains exactly the 36 effective values after:

```text
compiled defaults < TOML < CLI overrides
```

Fields retain their TOML names and JSON scalar types. `general.run_folder` is
`null` when omitted. Enum values use canonical TOML spellings such as `auto`
and `5GHz`. Configured paths are recorded as configured; resolved paths and
configuration provenance are not included.

## Default Output Name

The compiled default, `config/basic_config.toml`, public tests, help-facing
documentation, English and Russian README files, and nested `.gitignore` use:

```text
output.json
```

The old `mac-node-stats.json` default and ignore entry are removed. Explicit
`general.output_name` overrides continue to work unchanged.

## Error Handling

- Build both typed summaries before opening the output file.
- Exclusive creation remains authoritative at write time; an existing or
  race-winning output is never truncated.
- Opening, streaming, flushing, or closing failures throw `std::runtime_error`
  with the output path.
- Main catches summary/serialization exceptions, destroys simulator state,
  prints an error, returns 1, and does not print the successful completion
  banner.
- Summary builders do not emit report data to stdout or ns-3 logs.
- Existing per-event debug logging and ordinary startup/progress/completion
  output remain in scope and are not removed.

## Testing Strategy

### Defaults and configuration

- Assert `ScenarioConfig{}.general.outputName == "output.json"`.
- Assert the starter TOML assigns `output_name = "output.json"`.
- Assert help and both READMEs describe the new default.
- Assert the nested ignore rule is `/output.json` and no stale default remains.
- Assert the registry serializer produces all eight sections and exactly 36
  effective fields with correct string, optional-null, enum, integer, float,
  and Boolean types.
- Apply representative TOML and CLI overrides and assert metadata contains the
  final effective values.
- Include quotes, backslashes, and non-ASCII text in configured strings and
  parse the output to prove valid JSON escaping.

### Transmission summary

- Matched transmit/receive samples produce the expected positive match count,
  duration sum, byte total, and SI Mbps rate.
- A sender with bytes but no matched positive duration remains present with a
  null effective rate.
- Byte accumulation starts with `uint64_t` and covers a total larger than
  `INT_MAX` without overflow.
- No transmission summary banner or sender row is logged or printed.

### Cross-layer summary

- Registered nodes with no events still produce all-zero interval and overall
  rows.
- A partial final interval uses its actual duration.
- Delay count/mean/population standard deviation/minimum/maximum match literal
  samples.
- Per-interval and overall byte/count/rate/utilization fields match the current
  formulas.
- MAC drop reasons and per-agent application drops are preserved in structured
  arrays.
- No cross-layer report banner, interval row, overall row, or drop row is
  logged or printed.

### Complete JSON

- Parse the real streamed file with `nlohmann::json`.
- Assert `schema_version == 1` and every required root member exists.
- Assert every measurement field has its specified unit/count suffix.
- Assert removed ambiguous names do not appear as object keys.
- Assert Wi-Fi windows, Wi-Fi summary, transmission summary, cross-layer
  summary, validation, and configuration metadata contain representative
  literal values.
- Retain collision, missing-parent, flush/close-state, and 64-bit sparse-window
  coverage.
- Assert `experiment_metadata` is emitted after `validation` in the serialized
  text even though parsers treat object order as insignificant.

### Integration and documentation

- Build with examples, tests, logs, warnings, and warnings-as-errors enabled.
- Run the full `llm` suite and registered `llm_sample` smoke.
- Run a short public experiment, parse `output.json`, and verify the summaries
  and all 36 effective configuration fields.
- Capture executable output with scenario logging enabled and assert the two
  removed report banners and measurement-row prefixes are absent.
- Update README.md and README_RU.md together with identical schema keys,
  formulas, commands, and field tables.
- Run formatting, `git diff --check`, schema-key scans, file-size checks, and
  output/temp-artifact cleanup checks.

## Acceptance Criteria

- The default result is named `output.json`.
- One valid JSON file contains all Wi-Fi, transmission, and cross-layer
  measurements previously split between JSON and logs.
- End-of-experiment summaries are absent from stdout and ns-3 logs.
- Measurement field names state units or counts wherever applicable.
- The breaking schema uses the precise names in this specification and does
  not emit aliases for old keys.
- `experiment_metadata.configuration` is last and contains exactly all 36
  effective typed fields, with no resolved paths.
- Existing no-clobber and output error behavior remains correct.
- All focused and integration verification passes with no implementation file
  above 600 lines and no trace-data changes.
