# Unified Window Statistics Design

## Status

Approved in conversation on 2026-08-25. This specification replaces the
current experiment JSON hierarchy in place. Backward compatibility is not
required, and `schema_version` remains integer `1` by explicit decision.

## Goal

Collect every interval-based experiment measurement on the configured
`statistics.window_ms` clock and write one window-first JSON hierarchy:

```text
windows
  -> access_points (BSS aggregate)
  -> stations (per-STA detail)
       -> general_stats
       -> app_stats
       -> tcp_stats
       -> mac_stats
       -> phy_stats
```

Whole-experiment totals remain under a separate `overall` root with the same
AP/STA/category hierarchy. The central collector replaces fragmented Wi-Fi
windows, fixed one-second cross-layer buckets, generator-private one-second
maps, and the separate device transmission summary.

## Verification of the Updated Task

The updated task is technically correct with these scope corrections:

- `APGenerator` accepted-byte, agent, station, and CWND statistics currently
  exist only in a private one-second map and final log report.
- `StaLlmGenerator` has the same one-second map and additionally collects RTT
  samples that its report does not currently print.
- `cross_layer_summary` uses fixed one-second buckets even though the scenario
  exposes configurable `statistics.window_ms`.
- AP generator CWND is currently collapsed across several station sockets;
  a single value is not meaningful for independent TCP connections.
- Generator throughput assumes every bucket is exactly one second and cannot
  represent arbitrary or partial windows precisely.
- Receiver-side `TrafficSink` observations are not part of the JSON, leaving
  application statistics asymmetric.
- The current root separates Wi-Fi, transmission, and cross-layer results,
  requiring consumers to join different time bases and identity schemes.

The general rule is therefore valid: interval measurements use the configured
statistics window. Totals remain overall. Point events belong to one window;
durations and state values are split or time-weighted across every window they
span.

## Non-Goals

- Preserve the current `wifi_windows`, `wifi_summary`,
  `transmission_summary`, or `cross_layer_summary` schema.
- Increment the schema version or provide aliases/migration fields.
- Emit dense all-zero entities in every window.
- Average already-calculated window averages.
- Combine CWND or RTT values from independent TCP connections.
- Treat a RAR archive as a simulation input.
- Change traffic scheduling, topology placement, Wi-Fi standard, TCP
  congestion control, or the existing TOML configuration schema.
- Add resolved paths to metadata.

## Chosen Architecture

Use one unified experiment-window collector.

`WifiStatistics` is renamed and generalized to `ExperimentStatistics`. It
owns:

- AP/STA identity and BSS membership;
- the common experiment epoch and configurable window width;
- sparse per-window AP/STA accumulators;
- application, sink, TCP, MAC, PHY, and cross-layer observations;
- per-peer TCP state for CWND time integration;
- device TX/RX matching state;
- whole-experiment aggregation and validation;
- the streaming JSON serializer input.

`TrafficFlowMonitor` is removed. Its packet parsing, device TX/RX matching,
and estimated TCP payload accounting move into focused
`experiment-statistics-device.cc` code connected by the central owner.

AP/STA generators no longer store one-second metrics or print final reports.
They emit accepted-send, drop, CWND, and RTT events. `TrafficSink` continues to
emit receive events, and the central owner computes per-window receive and
inter-arrival statistics.

Large JSON arrays remain streamed. `nlohmann::json` is used only for scalar
encoding/escaping and the small effective-configuration object.

## Component Responsibilities

### Experiment statistics owner

The public owner is renamed to `ExperimentStatistics`. Its focused source
files are split by responsibility:

```text
experiment-statistics-owner.cc       construction, registration, public API
experiment-statistics-window.cc      time resolution and interval splitting
experiment-statistics-app.cc         generator, sink, agent, peer observations
experiment-statistics-tcp.cc         per-peer CWND integration and RTT samples
experiment-statistics-device.cc      MAC TX/RX parsing and matching
experiment-statistics-mac.cc         MAC drops and failures
experiment-statistics-phy.cc         tagged PHY attempts, rates, airtime, busy time
experiment-statistics-summary.cc     sparse windows, BSS parents, overall merge
experiment-statistics-json.cc        entity/category JSON serialization
experiment-json.cc                   root order, metadata, validation, file I/O
```

No non-vendored implementation file may exceed 600 lines.

### APGenerator

Remove `PerSecondStats`, `m_metricsByAbsoluteSecond`,
`PrintPerSecondMetrics()`, and `ap-generator-report.cc`.

The accepted-send trace reports the bytes actually accepted by `Socket::Send`,
not the requested payload size. It retains station peer and agent identity.

Add per-peer transport trace events containing station address, event time,
old/new congestion window bytes, and RTT sample microseconds. Each AP socket
connects both `CongestionWindow` and `LastRTT`. AP transport state is never
collapsed across station sockets.

### StaLlmGenerator

Remove `PerSecondStats`, `m_metricsByAbsoluteSecond`,
`PrintPerSecondMetrics()`, and `sta-llm-generator-report.cc`.

The accepted-send trace reports actual accepted bytes and agent identity. New
transport trace events report the AP peer, event time, CWND changes, and RTT
samples. The generator retains only socket state required to emit accurate
events; it does not aggregate output statistics.

### TrafficSink

`RxCustom` remains the source of received payload bytes. The owner binds sink
node identity and direction when connecting the trace. It records received
bytes/counts, per-peer data when the peer resolves, and inter-arrival samples
assigned to the later receive event's window. Last-receive time is tracked per
entity, direction, and peer so interleaved connections do not create false
cross-peer samples.

The old final `[Received Stats]` measurement log is removed. Ordinary
connection/debug/error logs remain.

### Device and Wi-Fi traces

The central owner connects device `MacTx`/`MacRx`, MAC drop/failure traces,
`PhyTxBegin`, `PhyTxPsduBegin`, PHY state, generator accepted-send/drop/TCP
events, and sink receive events.

Management/control frames without tagged or parseable TCP payload remain
excluded from payload accounting but still contribute to PHY busy time.

## Identity Model

The complete inventory is registered before the experiment:

```text
AP:  access_point_id, node_id, node_label, ipv4
STA: access_point_id, station_index, node_id, node_label, ipv4
```

`access_point_id` is the zero-based BSS identifier. `station_index` is
zero-based within that BSS. IP/node maps resolve observations to stable entity
records.

Arrays use deterministic order: AP ID; `(AP ID, station index)`; peer node/IP;
agent key; numeric reason code.

## AP as BSS Aggregate

An AP JSON object is the aggregate view of its BSS, not only the physical AP
node. Station objects are the child detail.

```text
STA 0 uplink accepted payload: 1000 bytes
STA 1 uplink accepted payload: 2000 bytes
AP 0 uplink accepted payload:  3000 bytes
```

Rules:

- AP downlink sender statistics originate from the AP generator/socket/PHY and
  are broken down by destination STA.
- AP uplink sender-side statistics merge associated STA uplink observations.
- AP uplink receiver statistics originate from the AP sink/device.
- AP downlink receiver statistics merge associated STA sink/device data.
- AP TCP arrays contain per-peer AP-to-STA downlink and STA-to-AP uplink
  connections.
- AP MAC/PHY directional payload totals aggregate BSS child flows; STA rows
  retain attributed detail.
- AP PHY busy time/utilization uses the AP PHY as the BSS channel reference and
  is not the sum of station busy times.

Parent/child duplication is intentional. Validation checks child sums only
where attribution is complete; it does not equate accepted and received bytes.

## Common Window Model

### Boundaries

All interval measurements use `statistics.window_ms`:

```text
window_us = window_ms * 1000
window_index = floor(relative_time_us / window_us)
window_start_ms = window_index * window_ms
last_duration_ms = min(window_ms, experiment_duration_ms - window_start_ms)
```

The last duration is positive for an emitted window. Use 64-bit indexes,
counts, and timestamps with overflow-safe ceiling division.

### Event ownership

Point observations belong to the window containing their timestamp. Device
TX/RX matches belong to the transmit event's window, even if receive occurs in
a later window.

### Duration/state splitting

PHY busy intervals and CWND step-function state are intersected with every
configured window they span. Only overlap microseconds are accumulated.

### Sparse output

- Completely empty windows and inactive entities are omitted.
- Once emitted, an entity contains every category and both directions.
- Empty fields use zero counts/bytes, empty arrays, and null undefined values.
- `overall` and metadata inventory contain every registered AP/STA.

## Average and Total Semantics

Never average per-window averages.

```text
window_throughput_mbps = payload_bytes * 8 / actual_window_duration_us
overall_throughput_mbps = total_payload_bytes * 8 / experiment_duration_us

average_phy_rate_mbps =
  sum(data_rate_bps * allocated_airtime_us) / total_airtime_us / 1e6

channel_utilization_percent =
  min(100, busy_time_us / denominator_duration_us * 100)

average_cwnd_bytes =
  sum(cwnd_bytes * observation_duration_us) / total_observation_duration_us
```

Delay, RTT, inter-arrival, and transmission-duration accumulators retain count,
sum, sum of squares, minimum, and maximum. Overall merges raw accumulators.
Population standard deviation is used.

Shares use child bytes divided by parent-direction bytes. Overall shares use
overall totals. Zero/unknown denominators or no samples/airtime/CWND
observation produce null. A known duration with zero bytes produces numeric
zero throughput.

CWND changes before the epoch seed state without accumulating time. Finalize
each known connection from its last update to experiment end before output.

## Root JSON Schema

The schema remains version `1` and is replaced in place:

```json
{
  "schema_version": 1,
  "measurement_semantics": {},
  "statistics_window_ms": 10,
  "windows": [],
  "overall": {
    "access_points": [],
    "stations": []
  },
  "validation": {},
  "experiment_metadata": {
    "configuration": {},
    "entity_inventory": {
      "access_points": [],
      "stations": []
    }
  }
}
```

Physical order matches the example; metadata remains last. Resolved paths and
field provenance are absent. Remove without aliases:

```text
wifi_windows
wifi_summary
transmission_summary
cross_layer_summary
```

## Window and Entity Schema

Each sparse window is:

```json
{
  "window_index": 42,
  "window_start_ms": 420,
  "window_duration_ms": 10,
  "access_points": [],
  "stations": []
}
```

The final duration may be shorter/fractional.

AP identity fields are `access_point_id`, `node_id`, `node_label`, and `ipv4`.
STA identity adds `station_index` and retains its parent `access_point_id`.

Every emitted/overall entity contains:

```json
{
  "general_stats": {"uplink": {}, "downlink": {}},
  "app_stats": {"uplink": {}, "downlink": {}},
  "tcp_stats": {"uplink": {}, "downlink": {}},
  "mac_stats": {"uplink": {}, "downlink": {}},
  "phy_stats": {
    "busy_time_us": 0,
    "channel_utilization_percent": 0.0,
    "uplink": {},
    "downlink": {}
  }
}
```

Non-directional measurements remain at category level and are never copied
into both directions.

## General Statistics

Each direction contains:

```text
estimated_transmitted_tcp_payload_bytes
estimated_matched_tcp_payload_bytes
matched_packet_count
total_transmission_duration_us
average_transmission_duration_us
transmission_duration_standard_deviation_us
minimum_transmission_duration_us
maximum_transmission_duration_us
effective_throughput_mbps
application_to_phy_delay
```

The duration-derived values and effective throughput are null without positive
matches. `application_to_phy_delay` contains `sample_count`, `average_us`,
`standard_deviation_us`, `minimum_us`, and `maximum_us`, with derived fields
null when count is zero.

Device payload names say `estimated` because parsing subtracts fixed
LLC/SNAP, IPv4, and TCP header sizes. Effective throughput uses matched bytes:

```text
effective_throughput_mbps =
  estimated_matched_tcp_payload_bytes * 8 /
  total_transmission_duration_us
```

## Application Statistics

Each direction contains:

```text
accepted_send_count
accepted_payload_bytes
accepted_throughput_mbps
receive_event_count
received_payload_bytes
received_throughput_mbps
drop_event_count
dropped_payload_bytes
receive_interarrival_time
agents
peers
```

`receive_interarrival_time` uses the standard sample-distribution shape.

Agent entries contain:

```text
agent_key
accepted_send_count
accepted_payload_bytes
accepted_throughput_mbps
accepted_bandwidth_share_percent
drop_event_count
dropped_payload_bytes
```

Emit an agent if it has an accepted send or drop in the window/overall.

Peer entries contain:

```text
peer_node_id
peer_ipv4
accepted_send_count
accepted_payload_bytes
accepted_throughput_mbps
accepted_bandwidth_share_percent
receive_event_count
received_payload_bytes
received_throughput_mbps
received_bandwidth_share_percent
drop_event_count
dropped_payload_bytes
```

This fixed shape supports sender and receiver directions without aliases.

## TCP Statistics

Each direction contains `connections`. Entries contain:

```text
peer_node_id
peer_ipv4
congestion_window_observation_duration_us
average_congestion_window_bytes
last_congestion_window_bytes
round_trip_time
```

`round_trip_time` uses the standard sample-distribution shape. CWND is
time-weighted per connection; RTT is sample-weighted. AP arrays retain every
station connection separately. A sender-side connection appears in the BSS AP
aggregate and the corresponding STA child direction; this intentional
duplication lets consumers use either parent totals or device detail.

## MAC Statistics

Each direction contains:

```text
estimated_transmit_event_count
estimated_transmitted_tcp_payload_bytes
estimated_transmit_throughput_mbps
estimated_receive_event_count
estimated_received_tcp_payload_bytes
estimated_receive_throughput_mbps
transmit_drop_count
transmit_drop_packet_bytes
mpdu_drop_count
mpdu_drop_bytes
data_failure_count
final_data_failure_count
mpdu_drops_by_reason
peers
```

Reason entries contain `reason_code` and `drop_count`. Peer entries contain
peer identity plus estimated transmit/receive counts, bytes, and throughputs.
Drop/failure fields remain direction totals when a peer cannot be resolved.

## PHY Statistics

Category-level fields are `busy_time_us` and
`channel_utilization_percent`. Each direction contains:

```text
tagged_payload_bytes
unique_tagged_payload_bytes
tagged_mpdu_count
complete_tagged_mpdu_bytes
transmission_attempt_count
retransmission_count
transmission_airtime_us
average_data_rate_mbps
throughput_mbps
peers
```

PHY peer entries contain peer identity plus the same directional payload,
attempt, retransmission, airtime, rate, and throughput fields. Management and
control airtime appears only through category-level busy time.

## Overall Schema

`overall.access_points` and `overall.stations` use the identical entity and
category shape. Throughput uses experiment duration; distributions merge raw
samples; CWND integrates over the experiment; PHY rate merges rate-airtime
products; shares use overall bytes; every inventory entity is present.

## Measurement Semantics

`measurement_semantics` documents:

- AP objects are BSS parent aggregates and STA objects are child detail;
- parent/child duplication is intentional;
- MAC TCP payload sizes are header-based estimates;
- PHY tagged payload includes attempts/retransmissions;
- unique PHY payload counts first tagged MPDU transmissions;
- PHY rate is airtime-weighted;
- CWND is time-weighted per connection;
- RTT/delay/inter-arrival values are sample-weighted;
- sparse absence means zero activity;
- undefined derived values are null.

## Validation

The root validation object contains:

```text
entity_inventory_references_valid
app_agent_totals_consistent
app_peer_totals_consistent
mac_peer_totals_consistent
phy_peer_totals_consistent
ap_station_sender_totals_consistent
overall_matches_windows
unique_phy_payload_within_tagged_payload
```

Compare raw totals, never rounded JSON doubles. Do not require accepted sender
bytes to equal received sink bytes because buffering, cutoff, and transport
behavior can differ.

## Metadata

`experiment_metadata.configuration` remains the registry-derived eight-section,
36-field effective configuration with exact TOML names/types and null omitted
run folder.

`entity_inventory` contains dense AP/STA identity arrays in deterministic
order. Inventory contains no statistics, resolved paths, or provenance.

## Error and Output Safety

- Finalize TCP state and build summaries before output creation.
- Continue `std::ios::noreplace`; never overwrite an existing/race-winning
  file.
- Propagate body, flush, and close failures with the output path.
- Main destroys simulator state and returns nonzero on finalization, summary,
  or serialization errors.
- No final APGenerator, StaLlmGenerator, TrafficSink, cross-layer, or device
  measurement report is printed to stdout/logs.
- Ordinary event, startup, progress, error, and completion logs remain.

## File and API Migration

Remove:

```text
examples/traffic-flow-monitor.*
model/ap-generator-report.cc
model/sta-llm-generator-report.cc
examples/wifi-statistics*.cc/.h
```

Replace Wi-Fi-statistics files with focused `experiment-statistics*` files.
Rename public owner use sites without compatibility aliases. Replace current
output DTOs with unified window/entity/category records; keep accumulators
private. Narrow internal test helpers are allowed, but mutable production state
is not public.

Update root/example CMake lists and split tests so implementation/test files
remain below 600 lines.

## Testing Strategy

### Time and window helpers

- first/last boundary inclusion;
- arbitrary 10 ms and 25 ms windows;
- partial final window;
- 64-bit index above `2^32`;
- duration splitter crossing several windows;
- empty/out-of-range events rejected.

### APP and sink collection

- AP downlink accepted bytes by agent and STA peer;
- STA uplink accepted bytes by agent;
- actual accepted bytes rather than requested bytes;
- AP uplink and STA downlink sink bytes/counts/inter-arrival;
- accepted/dropped agent/peer order and shares;
- zero denominators produce null shares;
- AP parent sender totals equal child STA totals where applicable.

### TCP collection

- AP downlink connections remain separate per STA;
- STA uplink connection references its AP;
- AP RTT collection exists;
- pre-epoch CWND seeds without accumulated duration;
- CWND spanning several windows is split/time-weighted;
- finalization flushes last state to experiment end;
- RTT samples use event windows and sample-weighted merge;
- empty connections produce null averages.

### Device, MAC, and PHY collection

- TX/RX match crossing a boundary belongs to transmit window;
- unmatched bytes remain while rate is null;
- AP/STA MAC TX/RX direction and peer mapping;
- drop/failure direction and reason ordering;
- tagged attempts, unique bytes, MPDU bytes, retransmissions, peer mapping;
- PHY busy intervals split across windows;
- airtime-weighted PHY rate and null without airtime;
- AP parent directional totals and STA child detail.

### Summary and JSON

- sparse windows/entities omitted;
- active entities emit fixed category/direction shape;
- overall includes every inventory entity;
- undefined averages null and known zero rates numeric zero;
- exact root order and metadata-last layout;
- exact unit/count suffixes and no old aliases;
- validation uses raw totals;
- configuration remains 8/36 and inventory references are valid;
- large output is streamed;
- no-clobber/output-error tests remain.

### Logging and documentation

- AP/STA generator and sink final report banners/rows absent;
- ordinary event/debug logging retained;
- English/Russian hierarchy, fields, formulas, examples, commands, links,
  and live-test instructions match;
- stale fixed-second and removed-root text absent.

## Live Trace Verification

Every `contrib/llm/traces/*.json` file is validated and run exactly once in a
temporary output directory after unit/registered tests pass.

| Trace | Policy |
|---|---|
| `1W_high_load_1s.json` | Full trace in auto mode, including configured tail |
| `1W_high_load_10s.json` | Full trace in auto mode, including configured tail |
| `1W_high_load_1m.json` | Fixed 1-second bounded live run |
| `1W_high_load_10m.json` | Fixed 1-second bounded live run |

The verification tool enumerates `traces/*.json` and fails when a discovered
file has no matrix entry. Every run uses one run only, a unique validated
`/tmp/llm-trace-live.*` directory, and no output-name override.

For each output, parse and assert:

- schema version 1 and tested configured trace path;
- nonempty `windows`;
- configured width except an allowed partial final duration;
- fixed five-category/two-direction shape for every emitted entity;
- AP/STA references exist in inventory;
- `overall` and all validation flags exist;
- old root aliases are absent;
- console has no legacy final report markers.

Remove temporary directories after success/failure. Final scans find no
`llm-trace-live.*`, task output, or task-created run folder. Pre-existing user
artifacts are reported and preserved.

## Documentation

Update `README.md` and `README_RU.md` together:

- explain window-first and AP-as-BSS-parent semantics simply;
- document every root, identity, category, direction, field, unit, null/zero,
  sparse, overall, validation, and metadata rule;
- include formulas and peer/agent ordering;
- state that APP/TCP/MAC/PHY/cross-layer intervals use
  `statistics.window_ms`;
- remove fixed-second and removed-root descriptions;
- include the live-test command/matrix;
- retain configuration/build/run guidance.

## Acceptance Criteria

- All interval measurements use configured windows; no AP/STA private
  one-second output map remains.
- JSON is window-first with sparse AP BSS parents and STA child details.
- Every emitted entity has fixed general/APP/TCP/MAC/PHY categories and both
  directions; non-directional PHY state is category-level.
- AP rows aggregate their BSS and applicable child totals validate.
- APP sender/receiver statistics are symmetric through generator/sink traces.
- TCP is per peer, RTT exists on AP/STA sender sockets, and CWND is
  time-weighted.
- General/device matches use transmit-window ownership and matched bytes for
  effective throughput.
- Overall uses raw accumulators and contains every entity.
- Undefined averages/shares are null; known zero rates are zero.
- No obsolete roots or final report logs remain; schema version stays 1.
- Effective configuration stays 8/36, inventory is complete, metadata last,
  and resolved paths absent.
- The default filename remains `output.json`; exclusive no-clobber and output
  error propagation remain intact.
- Unit, registered, public, and four live-trace runs pass without trace
  modification or task-created leftovers.
- Warnings-as-errors build, formatting, bilingual parity, no-clobber, and
  file-size limits pass.
