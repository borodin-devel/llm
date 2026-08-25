# ns-3 LLM Trace Replay Module

[Русская версия](README_RU.md)

This contributed ns-3 module replays communication recorded in large language
model (LLM) workload traces over an 802.11ax infrastructure network. It maps
application-level agents to access points (APs) and stations (STAs), opens TCP
connections, schedules trace-defined uplink and downlink payloads, and reports
what the application, MAC, and PHY layers observed.

The most important mental model is:

> This project does not run an LLM and does not simulate token generation.
> It replays the network timing and byte sizes produced by an LLM workload.

The default example creates three isolated basic service sets (BSSs), each
with one AP and 30 physical STAs. Many application agents may share one
physical STA.

## Contents

- [Goals and non-goals](#goals-and-non-goals)
- [How the model works](#how-the-model-works)
- [Default scenario](#default-scenario)
- [Trace format and field usage](#trace-format-and-field-usage)
- [Agent placement](#agent-placement)
- [Traffic timing and readiness barrier](#traffic-timing-and-readiness-barrier)
- [Application-to-PHY attribution](#application-to-phy-attribution)
- [Output metrics](#output-metrics)
- [IEEE 802.11 review](#ieee-80211-review)
- [Build and run](#build-and-run)
- [Large trace tools](#large-trace-tools)
- [Testing and debugging](#testing-and-debugging)
- [Project structure](#project-structure)
- [Glossary](#glossary)
- [Known limitations](#known-limitations)

## Goals and non-goals

### Primary goals

1. Replay LLM-agent communication with original trace timing.
2. Study how agent placement changes Wi-Fi contention.
3. Measure application payload as it moves through TCP, Wi-Fi MAC, and Wi-Fi
   PHY.
4. Separate first transmissions from retransmissions where possible.
5. Produce sparse, machine-readable per-BSS/per-STA statistics.
6. Keep large-trace preprocessing bounded in memory.
7. Make experiments repeatable with deterministic placement rules and fixed
   ns-3 RNG seed/run values.

### Non-goals

- Modeling LLM inference, token sampling, GPU execution, memory use, or model
  quality.
- Predicting real wall-clock inference latency. `durationMs` is taken from the
  input trace; it is not recomputed from `model`, `inputToken`, or
  `outputToken`.
- Certifying IEEE 802.11 conformance or Wi-Fi Alliance interoperability.
- Reproducing a regulatory-domain-specific channel plan.
- Modeling interference between the three default BSSs. They intentionally
  use separate channel objects.
- Enabling every 802.11ax feature. In particular, the scenario does not attach
  a multi-user scheduler, so OFDMA is not explicitly enabled.

## How the model works

```text
JSON trace
  -> parse agent operations and experiment duration
  -> estimate uplink overlap in fixed time slots
  -> assign agents to BSSs
  -> assign agents to physical STAs
  -> create APs, STAs, Wi-Fi devices, IP addresses, and TCP applications
  -> wait until every traffic generator has a connected TCP socket
  -> choose one common experiment epoch
  -> replay uplink/downlink payloads against that epoch
  -> carry application metadata through TCP with AppTxTag
  -> observe MAC/PHY attempts, airtime, rate, delay, retries, and drops
  -> write one window-first experiment JSON with unified AP/STA statistics
```

The implementation uses five main methods to achieve its goal:

1. **Trace reduction:** only fields needed for timing and byte replay enter the
   C++ model.
2. **Contention-aware placement:** overlapping uplink agents are distributed
   across BSSs, then grouped behind STAs according to a selectable policy.
3. **Global readiness barrier:** trace time zero is shared by every generator,
   independent of TCP connection order.
4. **Byte tagging:** `AppTxTag` follows application bytes when TCP splits or
   merges writes.
5. **Cross-layer accounting:** application callbacks, MAC traces, PHY traces,
   and PHY state are aggregated separately and compared.

## Default scenario

The executable is `llm_sample`. Its typed defaults are defined by
[`ScenarioConfig`](examples/scenario-config.h); a complete documented launch
configuration is in [`config/basic_config.toml`](config/basic_config.toml).

| Setting | Default | Meaning |
|---|---:|---|
| BSS count | 3 | Three independent AP groups |
| Physical STAs per BSS | 30 | 90 physical STAs total |
| Wi-Fi standard | 802.11ax | ns-3 `WIFI_STANDARD_80211ax` |
| Band | 5 GHz | `BAND_5GHZ` |
| Channel width | 20 MHz | TOML/CLI also accepts 40, 80, or 160 MHz |
| Channel number | 0 | ns-3 selects the first valid channel for the standard/band/width |
| Primary 20 MHz index | 0 | Lowest-frequency primary 20 MHz subchannel |
| Rate manager | MinstrelHt | Dynamic rate selection, including HE groups in current ns-3 |
| AP position for BSS `i` | `(100i, 100i, 100i)` m | Constant position |
| STA placement | 5 m uniform disc | Centered on the BSS AP |
| AP TCP sink port | 10000 | Receives STA uplink traffic |
| STA TCP sink ports | 9000 + STA index | Receive AP downlink traffic |
| Generator setup start | 1 s | Starts TCP connection setup, not payload replay |
| RNG seed/run | 12345 / 1 | Repeatable random placement |
| Statistics window | 10 ms | Unified sparse interval measurements |
| Automatic tail margin | 2 s | Added to trace duration in `auto` mode |

By default, each BSS uses its own `YansWifiChannel`.
`YansWifiChannelHelper::Default()` uses `ConstantSpeedPropagationDelayModel`
and `LogDistancePropagationLossModel`. With isolated channels, a transmission
in BSS 0 cannot interfere with a device in BSS 1 or 2, even if their nominal
channel settings or coordinates overlap. Setting
`topology.isolate_bss_channels = false` instead gives every BSS the same
channel object, so transmissions share the modeled medium.

Infrastructure association is real ns-3 MAC behavior: each BSS has a unique
SSID (`llm-ap-0`, `llm-ap-1`, or `llm-ap-2`), AP devices use `ApWifiMac`, STA
devices use `StaWifiMac`, and STAs perform active probing. Association events
are logged through the `Assoc` trace.

### Addressing

For BSS `i`:

```text
AP:      10.1.i.1:10000
STA 0:   10.1.i.2:9000
STA 1:   10.1.i.3:9001
...
STA 29:  10.1.i.31:9029
```

One AP generator serves all downlink agents in its BSS and maintains one TCP
socket per used physical STA. One STA generator serves all uplink agents
assigned to that STA and shares one TCP socket among those agents.

## Trace format and field usage

The C++ parser loads a JSON object with a top-level `traces` array. Large RAR
files must first be sliced with the streaming tools described below; the C++
parser itself uses a JSON DOM.

### Fields consumed by the C++ simulator

| JSON path | Required | Use |
|---|---|---|
| `traces[].agentId` | Yes | Numeric part of the agent key |
| `traces[].agentType` | Yes | String part of the agent key and deterministic type ID |
| `traces[].tasks` | Yes | Container iterated by the parser |
| `tasks[].operations` | Yes | Container iterated by the parser |
| `operations[].startOffsetMs` | Yes | Uplink send time relative to trace epoch, in ms |
| `operations[].durationMs` | Yes | Added to start time to obtain downlink send time |
| `operations[].uplinkBytes` | Yes | Application payload sent by the STA |
| `operations[].downlinkBytes` | Yes | Application payload sent by the AP |

The agent key is:

```text
<agentId>_<agentType>
```

For example, agent ID `7` with type `planner` becomes `7_planner`.

An operation is replayed as network traffic only when **both** byte fields are
positive. If either `uplinkBytes <= 0` or `downlinkBytes <= 0`, the operation
is not scheduled by the C++ traffic generators. It still contributes
`startOffsetMs + durationMs` to the parsed experiment duration.

### Timing example

Input:

```json
{
  "startOffsetMs": 100.0,
  "durationMs": 25.0,
  "uplinkBytes": 80,
  "downlinkBytes": 200
}
```

Replay behavior:

```text
trace time 100 ms: STA sends 80 uplink bytes to its AP
trace time 125 ms: AP sends 200 downlink bytes to that STA
```

`durationMs` therefore acts as the gap between request upload and response
download. It is not simulated compute time; it is replayed trace time.

### Fields preserved but ignored by the C++ simulator

Real traces also contain fields such as:

- root `metadata` (`source`, `generator`, device counts);
- trace `hostId` and `edgeDevice`;
- task `taskSequence`, `taskType`, `arrivalOffsetMs`, source-agent fields, and
  `callRef`;
- operation `opId`, `operationType`, `operatorCategory`, `name`, token counts,
  `model`, `serverHostId`, and `depend`.

The streaming scripts preserve these fields when creating slices. They rebase
`startOffsetMs` and numeric `arrivalOffsetMs`, and prune `depend` references to
removed operations. The C++ simulator currently ignores these extra fields.
In particular, token counts and model names do not affect payload size,
duration, placement, PHY rate, or any output metric.

### Numeric requirements

[`trace_stream.py`](scripts/trace_stream.py) applies the same important bounds
as the C++ parser:

- `agentId` must fit a signed 32-bit integer;
- byte fields must be integers in `[0, INT_MAX]`;
- offsets and durations must be finite and non-negative;
- `startOffsetMs + durationMs` must remain finite.

## Agent placement

The example uses `DistributeAgentsContentionAware()` in two phases.

### Phase 1: BSS assignment

Every network operation marks the fixed slot containing its uplink start. The
default slot width is 10 ms. Agents sharing uplink slots are considered to
overlap.

The algorithm repeatedly selects the unassigned agent with the most remaining
overlap and chooses the BSS that adds the fewest pairwise uplink conflicts.
Ties prefer:

1. fewer bytes already assigned to the BSS;
2. fewer agents already assigned;
3. lower BSS index.

This phase tries to separate overlapping agents across independent BSSs.

### Phase 2: STA assignment

The default example sets `lowContentionPriority = true`. It prefers placing an
agent behind a STA that is already active in the same uplink slots. Several
application agents then share one physical MAC contender instead of creating
several contending STAs.

Simple example:

```text
Agent A uses slots {10, 11}
STA 0 already uses slots {10, 11}
STA 1 already uses slots {20, 21}

Putting A on STA 0 adds 0 active STA/slot pairs.
Putting A on STA 1 adds 2 active STA/slot pairs.
The low-contention policy chooses STA 0.
```

When `lowContentionPriority = false`, the algorithm first seeds as many
physical STAs as possible with mutually low-affinity agents, then performs
affinity-aware placement.

The sample caps application agents per physical STA at 832. With 3 BSSs and
30 STAs per BSS, the configured total capacity is:

```text
3 * 30 * 832 = 74,880 application agents
```

This is not an IEEE 802.11 station limit. It is an application-level modeling
limit used by the placement algorithm.

## Traffic timing and readiness barrier

Starting an application at simulation time 1 s starts TCP setup only. Payload
events are not scheduled until every AP and used-STA generator reports that
its TCP connection is ready.

When the last generator becomes ready, `TrafficCoordinator` chooses the first
integer-second boundary strictly after the current time:

```text
all sockets ready at 1.37 s -> common trace epoch is 2.00 s
```

Every generator schedules trace offset zero against the same epoch. This
prevents early TCP connections from starting their workload before slower
connections.

Applications stop at:

```text
common epoch + configured experiment duration
```

Duration modes use milliseconds internally:

```text
auto:  maximum_duration_ms = trace_duration_ms + auto_tail_seconds * 1000
fixed: maximum_duration_ms = fixed_duration_seconds * 1000
```

A fixed duration that is shorter than the trace produces a truncation warning.

### TCP configuration caveat

The configured congestion-control TypeId is validated as a `TcpCongestionOps`
subclass and installed as the `TcpL4Protocol::SocketType` default before any
Internet stack is created. Segment size and send/receive buffer defaults are
also applied before applications create sockets. The default configuration
therefore uses `TcpHighSpeed`, 1460-byte segments, and 32 MiB buffers without
any later congestion-control replacement.

## Application-to-PHY attribution

TCP may split one application write into several segments or combine writes.
Packet identity alone is therefore insufficient for cross-layer accounting.

Before a socket send, the generator adds an `AppTxTag` byte tag containing:

- application packet UID;
- application transmit time in microseconds;
- source and destination IPv4 addresses;
- source and destination TCP ports;
- application payload bytes;
- agent key.

Because it is a byte tag, metadata follows the tagged byte range through TCP
segmentation and aggregation.

At PHY transmission:

1. tagged byte spans are found in the MPDU payload;
2. source/destination addresses determine BSS, STA, and direction;
3. tagged bytes are added to the configured statistics window;
4. the first observed transmission contributes unique bytes and
   application-to-PHY delay;
5. a repeated MPDU identity increments retransmission counters;
6. PPDU duration and nominal TXVECTOR rate are attributed to each represented
   flow.

For a PPDU containing multiple tagged flows, airtime is divided in proportion
to tagged bytes. Example:

```text
PPDU duration: 100 us
Flow A tagged bytes: 300
Flow B tagged bytes: 100

Flow A allocated airtime: 75 us
Flow B allocated airtime: 25 us
```

The nominal PHY rate is not application goodput. It is the data rate selected
in the `WifiTxVector` for an actual PPDU attempt.

## Output metrics

The experiment writes one window-first JSON document. `general.output_name`
selects its filename inside the run folder and defaults to `output.json`. The
writer creates the file exclusively: it never truncates or overwrites an
existing path.

The integer `schema_version` remains `1` by an explicit compatibility decision,
but this hierarchy is a breaking in-place replacement. There are no aliases for
the removed `wifi_windows`, `wifi_summary`, `transmission_summary`, or
`cross_layer_summary` roots, and there are no fixed-second interval records.
Consumers of the earlier version-1 document must migrate to the hierarchy
below.

All end-of-experiment measurements exist only in the JSON. The removed final
AP-generator, STA-generator, sink, transmission, and cross-layer report banners
and rows are not printed to stdout or ns-3 logs. Ordinary startup, event,
progress, error, and completion messages remain.

### Window-first model and root schema

`statistics.window_ms` is the one interval clock for application, TCP, device,
MAC, PHY, and cross-layer measurements. Output is sparse by window and entity:
an entirely inactive window is absent, and an inactive entity is absent from an
emitted window. Once an entity is emitted, however, it always has every fixed
category and both directions. `overall` is different: it is dense and contains
every registered AP and STA, including all-zero entities.

An AP record is the parent aggregate for its BSS, while STA records are child
device detail. Parent/child duplication is intentional. For example:

```text
STA 0 uplink accepted payload: 1000 bytes
STA 1 uplink accepted payload: 2000 bytes
AP 0 uplink accepted payload:  3000 bytes
```

The seven root fields are physically written in the order shown here:

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

| Root field | Unit | Meaning |
|---|---:|---|
| `schema_version` | integer | Schema version, exactly `1` |
| `measurement_semantics` | object | Machine-readable interpretation of parent/child, payload, average, sparse, and null behavior |
| `statistics_window_ms` | ms | Configured interval width |
| `windows` | array | Sparse configured-window records |
| `overall` | object | Dense whole-experiment AP/STA records |
| `validation` | object | Eight raw-total consistency flags |
| `experiment_metadata` | object | Effective configuration and dense entity inventory; written last |

`measurement_semantics` contains these exact fields:

| Field | Meaning |
|---|---|
| `access_point_role` | AP objects are `BSS parent aggregate` records |
| `station_role` | STA objects are `per-station child detail` records |
| `parent_child_duplication` | Parent/child duplication is `intentional` |
| `mac_tcp_payload_bytes` | MAC TCP payload values are `header-based estimates` |
| `phy_tagged_payload_bytes` | Attempts and retransmissions are included |
| `phy_unique_tagged_payload_bytes` | Only first tagged MPDU transmissions are included |
| `phy_average_data_rate` | The nominal rate is airtime-weighted |
| `congestion_window` | CWND is time-weighted per connection |
| `sample_distributions` | Delay, RTT, inter-arrival, and duration distributions are sample-weighted |
| `sparse_window_absence` | Absence means zero activity |
| `undefined_derived_values` | Always JSON `null` |

### Window boundaries, ownership, and identities

Window boundaries are relative to the common experiment epoch:

```text
window_us = statistics_window_ms * 1000
window_index = floor(relative_time_us / window_us)
window_start_ms = window_index * statistics_window_ms
window_duration_ms = min(statistics_window_ms,
                         experiment_duration_ms - window_start_ms)
```

`window_duration_ms` is the actual positive duration. It normally equals the
configured width; a final partial window can be shorter or fractional. For a
25 ms width and 35 ms experiment, the two possible window durations are 25 ms
and 10 ms. Every rate and utilization value uses the actual duration.

Point observations belong to the window containing their timestamp. This
includes accepted sends, application drops and receives, RTT samples, MAC
drops/failures, and PHY attempts. A matched device TX/RX measurement belongs to
the transmit event's window even when RX occurs later. Receive inter-arrival is
recorded in the later receive event's window and is tracked independently by
entity, direction, and peer. First-transmission application-to-PHY delay belongs
to the sender direction and the window containing that PHY attempt.

Duration/state observations are not assigned wholly to their start window. PHY
busy intervals and stepwise CWND state are intersected with every configured
window they span, and only the overlap microseconds enter each window. CWND
changes before the epoch seed the initial value without adding pre-epoch time;
the final state is integrated through experiment end.

Each window has this exact shape:

```json
{
  "window_index": 42,
  "window_start_ms": 420,
  "window_duration_ms": 10,
  "access_points": [],
  "stations": []
}
```

| Window field | Unit | Meaning |
|---|---:|---|
| `window_index` | index | Zero-based configured-window index |
| `window_start_ms` | ms | Start relative to the experiment epoch |
| `window_duration_ms` | ms | Actual full or partial duration |
| `access_points` | array | Active AP BSS-parent aggregates, ordered by `access_point_id` |
| `stations` | array | Active STA child records, ordered by `(access_point_id, station_index)` |

AP identity fields are `access_point_id` (zero-based BSS ID), `node_id`
(ns-3 node ID), `node_label` (stable label), and `ipv4` (address string). STA
identity adds `station_index`, zero-based within its BSS, and retains the parent
`access_point_id`. Peers are ordered by node ID/IP, agents by `agent_key`, and
MAC reasons by numeric `reason_code`.

The BSS-parent merge rules are:

- AP downlink sender statistics originate at the AP and are detailed by
  destination STA.
- AP uplink sender statistics merge associated STA uplink senders.
- AP uplink receiver statistics originate at the AP sink/device.
- AP downlink receiver statistics merge associated STA sink/device data.
- AP TCP arrays retain independent AP-to-STA and STA-to-AP peer connections.
- AP directional MAC/PHY payload totals aggregate BSS child flows; STA records
  retain their attributed detail.
- AP PHY busy time and utilization use only the AP PHY as the BSS reference;
  station busy times are not summed into it.

### Fixed entity shape and derived-value rules

Every emitted and overall entity has the identity fields above plus this fixed
shape:

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

`uplink` means STA -> AP and `downlink` means AP -> STA. PHY busy/utilization
is local, non-directional state and therefore stays at category level.

Counts, bytes, durations, and known rates with no activity are numeric zero;
breakdown arrays are empty. A derived average, share, rate, minimum, maximum,
or deviation is `null` when its denominator or sample set is undefined. A
known positive duration with zero payload produces numeric `0.0` throughput,
not `null`. Values are derived from raw accumulators; calculated window
averages are never averaged again.

The common formulas are:

```text
window_throughput_mbps = payload_bytes * 8 / actual_window_duration_us
overall_throughput_mbps = total_payload_bytes * 8 / experiment_duration_us

sample_average_us = sum(sample_us) / sample_count
sample_population_standard_deviation_us =
  sqrt(sum((sample_us - sample_average_us)^2) / sample_count)

bandwidth_share_percent = child_payload_bytes / direction_payload_bytes * 100

average_phy_rate_mbps =
  sum(data_rate_bps * allocated_airtime_us) / total_airtime_us / 1e6

channel_utilization_percent =
  min(100, busy_time_us / denominator_duration_us * 100)

average_congestion_window_bytes =
  sum(cwnd_bytes * observation_duration_us) / total_observation_duration_us
```

Bytes multiplied by eight and divided by microseconds directly produce Mbps.
Shares are calculated separately for accepted and received direction totals;
overall shares use whole-experiment bytes.

### `general_stats`

Each direction contains:

| Field | Unit | Meaning |
|---|---:|---|
| `estimated_transmitted_tcp_payload_bytes` | bytes | Parsed TCP payload in device `MacTx` enqueue observations |
| `estimated_matched_tcp_payload_bytes` | bytes | Payload in positive-duration TX/RX matches |
| `matched_packet_count` | count | Positive-duration matched packets |
| `total_transmission_duration_us` | us | Sum of matched RX minus TX durations |
| `average_transmission_duration_us` | us or `null` | Arithmetic mean matched duration |
| `transmission_duration_standard_deviation_us` | us or `null` | Population deviation of matched durations |
| `minimum_transmission_duration_us` | us or `null` | Minimum positive matched duration |
| `maximum_transmission_duration_us` | us or `null` | Maximum positive matched duration |
| `effective_throughput_mbps` | Mbps or `null` | Matched payload rate over matched duration |
| `application_to_phy_delay` | object | First-transmission application-to-PHY sample distribution |

Payload is estimated after parsing LLC/SNAP, IPv4, and TCP headers. Device
`MacTx` observes a packet enqueue into the Wi-Fi MAC. A TCP-retransmitted packet
can therefore appear as a new enqueue, but an 802.11 retry of an already
enqueued MPDU is not another device `MacTx` event; Wi-Fi attempts and retries
are measured by `phy_stats`. Matching uses addresses, ports, estimated payload
size, and observation order. It is a diagnostic, not an ns-3 `FlowMonitor`
metric. The category-specific formula is:

```text
effective_throughput_mbps =
  estimated_matched_tcp_payload_bytes * 8 / total_transmission_duration_us
```

With no positive match, the count, bytes, and total duration are zero while all
four duration-derived fields and `effective_throughput_mbps` are `null`.

Every sample distribution (`application_to_phy_delay`,
`receive_interarrival_time`, and `round_trip_time`) has:

| Field | Unit | Meaning |
|---|---:|---|
| `sample_count` | count | Number of raw samples |
| `average_us` | us or `null` | Arithmetic sample mean |
| `standard_deviation_us` | us or `null` | Population standard deviation |
| `minimum_us` | us or `null` | Minimum sample |
| `maximum_us` | us or `null` | Maximum sample |

All four derived fields are `null` when `sample_count` is zero.

### `app_stats`

Each direction contains:

| Field | Unit | Meaning |
|---|---:|---|
| `accepted_send_count` | count | `Socket::Send` calls accepting positive payload |
| `accepted_payload_bytes` | bytes | Bytes actually accepted by TCP, not requested bytes |
| `accepted_throughput_mbps` | Mbps or `null` | Accepted bytes over the window/experiment duration |
| `receive_event_count` | count | Sink receive callbacks |
| `received_payload_bytes` | bytes | Bytes delivered to the sink |
| `received_throughput_mbps` | Mbps or `null` | Received bytes over the window/experiment duration |
| `drop_event_count` | count | Failed or partially rejected application sends |
| `dropped_payload_bytes` | bytes | Payload not accepted by TCP |
| `receive_interarrival_time` | object | Per-peer receive inter-arrival distribution |
| `agents` | array | Sender/drop breakdown ordered by `agent_key` |
| `peers` | array | Sender/receiver/drop breakdown ordered by peer |

An `agents` entry contains:

| Field | Unit | Meaning |
|---|---:|---|
| `agent_key` | string | Stable trace-agent key |
| `accepted_send_count` | count | Accepted sends for the agent |
| `accepted_payload_bytes` | bytes | Accepted bytes for the agent |
| `accepted_throughput_mbps` | Mbps or `null` | Agent accepted rate |
| `accepted_bandwidth_share_percent` | percent or `null` | Agent accepted bytes divided by direction accepted bytes |
| `drop_event_count` | count | Agent application drops |
| `dropped_payload_bytes` | bytes | Agent dropped payload |

A `peers` entry contains `peer_node_id` and `peer_ipv4`, followed by
`accepted_send_count`, `accepted_payload_bytes`, `accepted_throughput_mbps`,
`accepted_bandwidth_share_percent`, `receive_event_count`,
`received_payload_bytes`, `received_throughput_mbps`,
`received_bandwidth_share_percent`, `drop_event_count`, and
`dropped_payload_bytes`, with the same units and meanings as the direction or
agent fields. Accepted and received values are deliberately not required to be
equal because buffering, cutoff, and TCP behavior can differ.

### `tcp_stats`

Each direction has a `connections` array. Every per-peer connection contains:

| Field | Unit | Meaning |
|---|---:|---|
| `peer_node_id` | node ID | Inventory peer reference |
| `peer_ipv4` | IPv4 address | Inventory peer address |
| `congestion_window_observation_duration_us` | us | Total duration with an observed CWND state |
| `average_congestion_window_bytes` | bytes or `null` | Time-weighted CWND |
| `last_congestion_window_bytes` | bytes or `null` | Last observed CWND |
| `round_trip_time` | object | RTT sample distribution |

CWND is never combined across independent peer sockets. Its average is `null`
without positive observation duration, its last value is `null` when none was
observed, and RTT-derived fields are `null` without samples.

### `mac_stats`

Each direction contains:

| Field | Unit | Meaning |
|---|---:|---|
| `estimated_transmit_event_count` | count | Parsed payload-bearing device `MacTx` enqueue observations |
| `estimated_transmitted_tcp_payload_bytes` | bytes | Estimated TCP payload in those enqueue observations |
| `estimated_transmit_throughput_mbps` | Mbps or `null` | Estimated TX payload rate |
| `estimated_receive_event_count` | count | Parsed MAC RX payload events |
| `estimated_received_tcp_payload_bytes` | bytes | Estimated received TCP payload |
| `estimated_receive_throughput_mbps` | Mbps or `null` | Estimated RX payload rate |
| `transmit_drop_count` | count | MAC transmit-drop callbacks |
| `transmit_drop_packet_bytes` | bytes | Whole packet bytes carried by transmit-drop callbacks |
| `mpdu_drop_count` | count | Dropped MPDUs |
| `mpdu_drop_bytes` | bytes | Whole bytes of dropped MPDUs |
| `data_failure_count` | count | MAC data-failure callbacks |
| `final_data_failure_count` | count | Final MAC data-failure callbacks |
| `mpdu_drops_by_reason` | array | `{reason_code, drop_count}` totals in numeric reason order |
| `peers` | array | Resolved per-peer MAC detail |

A MAC peer starts with `peer_node_id` and `peer_ipv4`, then contains
`estimated_transmit_event_count`, `estimated_transmitted_tcp_payload_bytes`,
`estimated_transmit_throughput_mbps`, `estimated_receive_event_count`,
`estimated_received_tcp_payload_bytes`, `estimated_receive_throughput_mbps`,
`mpdu_drop_count`, `mpdu_drop_bytes`, `data_failure_count`,
`final_data_failure_count`, and `mpdu_drops_by_reason`. Units match the
direction table. Observations whose peer cannot be resolved remain only in the
direction totals.

### `phy_stats`

Category-level fields are:

| Field | Unit | Meaning |
|---|---:|---|
| `busy_time_us` | us | Local PHY TX, RX, and CCA_BUSY overlap with the denominator duration |
| `channel_utilization_percent` | percent or `null` | Busy-time share, capped at 100 percent |

Each direction contains:

| Field | Unit | Meaning |
|---|---:|---|
| `tagged_payload_bytes` | bytes | Tagged bytes over all attempts, including retransmissions |
| `unique_tagged_payload_bytes` | bytes | Tagged bytes on first observed MPDU transmission only |
| `tagged_mpdu_count` | count | Complete tagged MPDU attempts |
| `complete_tagged_mpdu_bytes` | bytes | Whole bytes of complete tagged MPDU attempts |
| `transmission_attempt_count` | count | Tagged transmission attempts |
| `retransmission_count` | count | Repeated tagged MPDU identities |
| `transmission_airtime_us` | us | PPDU airtime allocated in proportion to tagged bytes |
| `average_data_rate_mbps` | Mbps or `null` | Airtime-weighted nominal `WifiTxVector` rate |
| `throughput_mbps` | Mbps or `null` | Tagged payload rate over the denominator duration |
| `peers` | array | Resolved per-peer PHY detail |

A PHY peer contains `peer_node_id`, `peer_ipv4`, `tagged_payload_bytes`,
`unique_tagged_payload_bytes`, `transmission_attempt_count`,
`retransmission_count`, `transmission_airtime_us`,
`average_data_rate_mbps`, and `throughput_mbps`, with the same units and
meaning. Complete MPDU count/bytes remain direction-level because a single
MPDU can carry several tagged peer spans. Control and management airtime
affects local `busy_time_us` but has no tagged peer payload.

`average_data_rate_mbps` is `null` without allocated airtime.
`channel_utilization_percent` and `throughput_mbps` are `null` only without a
valid denominator; over a valid duration, zero busy time or payload produces
numeric zero.

### Dense overall, validation, and metadata

`overall.access_points` and `overall.stations` use exactly the same identity,
category, direction, and nested field shapes. They contain every inventory
entity. Raw window accumulators are merged before deriving results: throughput
uses total experiment duration, distributions merge raw samples, PHY rate
merges rate-airtime products, CWND merges bytes-duration products, and shares
use whole-experiment direction bytes.

All eight `validation` fields must be `true` for a valid run:

| Field | Check |
|---|---|
| `entity_inventory_references_valid` | Every entity and resolved peer references inventory identity/address data |
| `app_agent_totals_consistent` | Agent accepted/drop totals fit their application direction totals |
| `app_peer_totals_consistent` | Resolved peer totals fit application direction totals |
| `mac_peer_totals_consistent` | MAC peer and reason totals are consistent with direction totals |
| `phy_peer_totals_consistent` | PHY peer-attributed raw totals match direction totals where attribution is complete |
| `ap_station_sender_totals_consistent` | Reconstructed AP BSS-parent sender totals match AP-local and STA-child raw data |
| `overall_matches_windows` | Dense overall raw values equal the merge of all configured windows, including omitted empty ones |
| `unique_phy_payload_within_tagged_payload` | Unique PHY payload never exceeds attempted tagged payload |

Validation compares integer/raw accumulator values before JSON rounding. It
does not compare accepted sender bytes with received sink bytes.

`experiment_metadata.entity_inventory` is dense, statistics-free, and
deterministically ordered. AP entries contain `access_point_id`, `node_id`,
`node_label`, and `ipv4`; STA entries also contain `station_index`.

`experiment_metadata.configuration` records the final registry values after:

```text
compiled defaults < TOML values < CLI overrides
```

The exact eight sections and 36 output fields are:

| Object | Effective fields |
|---|---|
| `general` | `trace_file`, `run_folder`, `output_name` |
| `simulation` | `duration_mode`, `fixed_duration_seconds`, `auto_tail_seconds`, `rng_seed`, `rng_run` |
| `topology` | `bss_count`, `stations_per_bss`, `bss_spacing_m`, `station_radius_m`, `isolate_bss_channels`, `ssid_prefix`, `ap_sink_port`, `station_sink_base_port`, `generator_start_seconds` |
| `distribution` | `max_agents_per_station`, `low_contention_priority`, `slot_ms` |
| `wifi` | `band`, `channel_number`, `bandwidth_mhz`, `primary_20_index`, `rate_manager`, `active_probing` |
| `tcp` | `congestion_control`, `segment_size_bytes`, `send_buffer_bytes`, `receive_buffer_bytes` |
| `statistics` | `window_ms` |
| `logging` | `sample_scenario_level`, `ap_generator_level`, `sta_generator_level`, `traffic_sink_level`, `contention_distribution_level` |

The checked-in [`config/basic_config.toml`](config/basic_config.toml) has eight
sections and 35 assigned values because `run_folder` is deliberately shown as
a commented optional setting. Output always has all 36 fields and records an
omitted `general.run_folder` as `null`. Values preserve TOML names and JSON
scalar types; enum strings use canonical TOML spelling. Configured path strings
are retained exactly. Metadata contains no resolved paths, output path,
configuration provenance, or statistics.

### Reproducible live verification

After building `llm_sample`, the live tool validates discovery against an
exact four-trace policy, validates each input with `find_window.py`, then runs
one simulation per trace sequentially. It uses a unique
`/tmp/llm-trace-live.<trace>.<random>` run folder, the default `output.json`,
the policy timeout, and no output-name override.

| Trace | Mode | Simulated range | Timeout |
|---|---|---:|---:|
| `1W_high_load_1s.json` | `auto` | full trace plus configured tail | 900 s |
| `1W_high_load_10s.json` | `auto` | full trace plus configured tail | 3600 s |
| `1W_high_load_1m.json` | `fixed` | 1.0 s | 1800 s |
| `1W_high_load_10m.json` | `fixed` | 1.0 s | 1800 s |

Run its deterministic pure-function/error/cleanup tests, then the real matrix:

```bash
python3 contrib/llm/scripts/live_test_traces.py --self-test
python3 contrib/llm/scripts/live_test_traces.py
```

The tool requires discovered and policy filenames to be equal, checks the
exact hierarchy, inventory-backed dense overall, all validation flags, 8/36
configuration, configured trace path, removed-root absence, and legacy-report
absence. On failure it prints the trace, command, return code, and last 200
console lines. Each validated task-created temporary directory is removed in a
`finally` path after success, command failure, timeout, or parse/shape failure;
unrelated run directories are never removed.

## IEEE 802.11 review

### What is correctly modeled/configured

| Area | Review |
|---|---|
| Standard selection | `WIFI_STANDARD_80211ax` selects ns-3 HE PHY/MAC behavior |
| Infrastructure topology | AP and non-AP STA MACs with SSID association |
| Channel widths | 20/40/80/160 MHz are schema choices; ns-3 validates the final standard/band/channel tuple |
| Channel tuple | `{number, width, band, primary20}` follows ns-3 `ChannelSettings` semantics |
| Frame exchange | ns-3 MAC/PHY handles data frames, ACK behavior, queues, aggregation, retries, and rate selection |
| PHY timing | PPDU duration is calculated by the active PHY entity and TXVECTOR |
| Medium state | TX, RX, and CCA_BUSY come from `WifiPhyStateHelper` |
| Association | `StaWifiMac` performs association; active probing is configurable |

Channel number zero does not mean IEEE channel 0. In ns-3 it means
"unspecified"; after the standard, band, and width are known, ns-3 selects the
first valid channel. For example, a concrete 20 MHz 5 GHz deployment would
usually name a regulatory channel such as 36, while this scenario deliberately
uses the ns-3 default.

### Important modeling choices and deviations

1. **Not a conformance test.** ns-3 implements substantial 802.11 behavior but
   is an abstract discrete-event model. Passing this scenario does not prove
   compliance of hardware or firmware.
2. **Default inter-BSS isolation.** The default independent
   `YansWifiChannel` objects are a stronger isolation assumption than merely
   assigning different IEEE channels. Set `isolate_bss_channels = false` to
   use one shared modeled channel, while retaining the Yans abstraction.
3. **No regulatory domain.** Country code, DFS, transmit limits, and channel
   availability are not configured.
4. **No explicit OFDMA scheduler.** The AP does not aggregate an ns-3
   multi-user scheduler, so HE multi-user/OFDMA operation is not enabled by
   this scenario.
5. **No explicit MU-MIMO, beamforming, BSS coloring, OBSS-PD spatial reuse,
   TWT, or security configuration.** Selecting 802.11ax does not automatically
   exercise all amendment features.
6. **Yans PHY abstraction.** Default Yans PHY is packet-level, not
   frequency-selective, and uses analytical reception/error models. It does
   not model walls, detailed campus geometry, or interference from non-Wi-Fi
   technologies.
7. **Rate control is model-dependent.** MinstrelHt supports HE groups in the
   current source tree, but ns-3 documentation lists known MinstrelHt and
   802.11ax modeling issues. Results are sensitive to ns-3 version.
8. **Processing delays are not an 802.11 mechanism here.** Trace duration
   controls when downlink data is released; ns-3 Wi-Fi processing delay is not
   being derived from the standard.

The configuration is therefore suitable for controlled **802.11ax-mode
network experiments with isolated or shared-channel BSSs**, but should not be
described as full IEEE 802.11ax compliance.

Official references:

- [ns-3 Wi-Fi design, scope, and limitations](https://www.nsnam.org/docs/models/html/wifi-design.html)
- [ns-3 Wi-Fi user documentation and channel settings](https://www.nsnam.org/docs/models/html/wifi-user.html)
- [IEEE 802.11 Working Group](https://www.ieee802.org/11/)

## Build and run

Commands are run from the ns-3 root, not from `contrib/llm`.

### Dependencies

The ns-3 build needs a C++ toolchain, CMake, Ninja, and Python. Large archived
trace processing additionally needs `unrar` and `python3-ijson`. Git LFS is
needed to fetch the tracked trace datasets.

Example apt-only installation:

```bash
sudo apt install build-essential cmake ninja-build ccache python3 \
  python3-ijson unrar git-lfs
git lfs install
git lfs pull
```

### Configure and build

```bash
./ns3 configure \
  --enable-examples \
  --enable-tests \
  --enable-logs \
  --enable-warnings \
  --enable-werror

./ns3 build llm-test llm_sample
```

### Run a small example

```bash
./ns3 run \
  "llm_sample --config contrib/llm/config/basic_config.toml"
```

Every real launch requires exactly one `--config PATH`. `--config` and all
overrides are position-independent, and each may appear only once. Show the
complete generated help without a configuration file:

```bash
./ns3 run "llm_sample --help"
```

The merged value order is:

```text
compiled defaults < TOML values < CLI overrides
```

Override names are section-prefixed kebab-case forms of the TOML keys.
Boolean overrides require the exact lowercase values `true` or `false`.
Unknown fields/flags, duplicate flags, positional arguments, wrong scalar
types, and invalid cross-field combinations stop startup with an error and
usage.

### Complete TOML and CLI reference

The eight TOML sections contain 36 fields. The shipped configuration assigns
35; optional `general.run_folder` is deliberately commented to demonstrate
automatic run-directory creation.

#### `[general]`

| TOML field | CLI override | Default / meaning |
|---|---|---|
| `trace_file` | `--general-trace-file` | Required input JSON trace |
| `run_folder` | `--general-run-folder` | Omitted; exact output directory when set |
| `output_name` | `--general-output-name` | `output.json`; plain `.json` filename |

#### `[simulation]`

| TOML field | CLI override | Default / meaning |
|---|---|---|
| `duration_mode` | `--simulation-duration-mode` | `auto`; also accepts `fixed` |
| `fixed_duration_seconds` | `--simulation-fixed-duration-seconds` | `0.0`; must be positive in fixed mode |
| `auto_tail_seconds` | `--simulation-auto-tail-seconds` | `2.0`; non-negative tail after trace end |
| `rng_seed` | `--simulation-rng-seed` | `12345`; valid ns-3 seed range is 1 through 4294944442 |
| `rng_run` | `--simulation-rng-run` | `1`; deterministic run/substream number |

#### `[topology]`

| TOML field | CLI override | Default / meaning |
|---|---|---|
| `bss_count` | `--topology-bss-count` | `3`; range 1 through 256 for `10.1.<BSS>.0/24` |
| `stations_per_bss` | `--topology-stations-per-bss` | `30`; range 1 through 253 for hosts `.2` through `.254` |
| `bss_spacing_m` | `--topology-bss-spacing-m` | `100.0` m increment on X, Y, and Z |
| `station_radius_m` | `--topology-station-radius-m` | `5.0` m uniform STA-disc radius |
| `isolate_bss_channels` | `--topology-isolate-bss-channels` | `true`; one channel object per BSS, or one shared object when false |
| `ssid_prefix` | `--topology-ssid-prefix` | `llm-ap-`; prefix plus largest BSS index must be at most 32 bytes |
| `ap_sink_port` | `--topology-ap-sink-port` | `10000`; uplink TCP sink on every AP |
| `station_sink_base_port` | `--topology-station-sink-base-port` | `9000`; STA index is added for downlink sinks |
| `generator_start_seconds` | `--topology-generator-start-seconds` | `1.0`; TCP setup start, not payload start |

#### `[distribution]`

| TOML field | CLI override | Default / meaning |
|---|---|---|
| `max_agents_per_station` | `--distribution-max-agents-per-station` | `832`; zero means unlimited |
| `low_contention_priority` | `--distribution-low-contention-priority` | `true`; false maximizes initial STA use |
| `slot_ms` | `--distribution-slot-ms` | `10` ms uplink-overlap slot |

#### `[wifi]`

| TOML field | CLI override | Default / meaning |
|---|---|---|
| `band` | `--wifi-band` | `5GHz`; also accepts `2.4GHz` and `6GHz` |
| `channel_number` | `--wifi-channel-number` | `0`; ns-3 selects the first valid channel |
| `bandwidth_mhz` | `--wifi-bandwidth-mhz` | `20`; accepts 20, 40, 80, or 160 MHz |
| `primary_20_index` | `--wifi-primary-20-index` | `0`; index within the configured width |
| `rate_manager` | `--wifi-rate-manager` | `ns3::MinstrelHtWifiManager` TypeId |
| `active_probing` | `--wifi-active-probing` | `true`; active SSID probing by STAs |

The Wi-Fi standard remains fixed to 802.11ax; these fields configure that
scenario rather than selecting an arbitrary standard.

#### `[tcp]`

| TOML field | CLI override | Default / meaning |
|---|---|---|
| `congestion_control` | `--tcp-congestion-control` | `ns3::TcpHighSpeed` TypeId |
| `segment_size_bytes` | `--tcp-segment-size-bytes` | `1460` bytes |
| `send_buffer_bytes` | `--tcp-send-buffer-bytes` | `33554432` bytes |
| `receive_buffer_bytes` | `--tcp-receive-buffer-bytes` | `33554432` bytes |

Transport remains fixed to TCP. The configured congestion control and socket
defaults are applied before Internet-stack and socket creation.

#### `[statistics]`

| TOML field | CLI override | Default / meaning |
|---|---|---|
| `window_ms` | `--statistics-window-ms` | `10` ms unified sparse statistics window |

#### `[logging]`

| TOML field | CLI override | Default / meaning |
|---|---|---|
| `sample_scenario_level` | `--logging-sample-scenario-level` | `info` |
| `ap_generator_level` | `--logging-ap-generator-level` | `warn` |
| `sta_generator_level` | `--logging-sta-generator-level` | `warn` |
| `traffic_sink_level` | `--logging-traffic-sink-level` | `warn` |
| `contention_distribution_level` | `--logging-contention-distribution-level` | `info` |

Every log field accepts `off`, `error`, `warn`, `info`, `debug`, `function`,
`logic`, or `all`. `off` makes no enable call, so it preserves logging already
enabled through `NS_LOG`.

### Path and output rules

The process current working directory is captured once at startup. Relative
config paths, `general.trace_file`, and `general.run_folder` all resolve
against that same directory, never against the directory containing the TOML
file.

- With an explicit `general.run_folder`, that path is the exact output
  directory. Missing parents are created and an existing directory is
  allowed.
- Without `general.run_folder`, the program creates
  `./run/YY-MM-DD_hh-mm-ss` using local launch time. The parent `./run` may be
  created, but an existing timestamp directory is a collision and startup is
  refused.
- `general.output_name` must be a plain `.json` filename, not a path. The
  final output is `<run_folder>/<output_name>` and an existing file is never
  overwritten.

Validation, path resolution, and directory preparation finish before ns-3
topology objects are created. Startup prints the resolved config, trace, run,
and output paths plus the major duration, topology, distribution, Wi-Fi, TCP,
RNG, and statistics choices. End-of-experiment measurement summaries are
written only to the printed output file. Ordinary startup, progress, error,
and completion messages and per-event/debug logging remain available. Its
metadata preserves configured path strings and does not copy these resolved
paths into the JSON.

Fixed-duration example:

```bash
./ns3 run \
  "llm_sample --config contrib/llm/config/basic_config.toml \
  --general-trace-file contrib/llm/traces/1W_high_load_10m.json \
  --general-run-folder build/high-load \
  --general-output-name high-load.json \
  --wifi-bandwidth-mhz 80 \
  --simulation-duration-mode fixed \
  --simulation-fixed-duration-seconds 600"
```

The 10-minute high-load trace is large and may require substantial simulation
time and memory.

## Large trace tools

The tracked RAR files expand to multi-gigabyte JSON documents. Do not extract
and load them with Python `json.load()` or pass them directly to `llm_sample`.
Use the bounded-memory streaming CLI.

### Validate JSON or RAR

```bash
PYTHONDONTWRITEBYTECODE=1 python3 contrib/llm/scripts/find_window.py \
  validate contrib/llm/traces/1W_端侧优先_tw6m_s42_w10000_st1000_mp_window_detailed_trace_w349000-359000.rar
```

### Write the first fitting minute

```bash
PYTHONDONTWRITEBYTECODE=1 python3 contrib/llm/scripts/find_window.py \
  slice-first INPUT.rar build/first-minute.json --window-seconds 60
```

### Find the highest-byte 10-minute window

```bash
PYTHONDONTWRITEBYTECODE=1 python3 contrib/llm/scripts/find_window.py \
  find-window INPUT.rar build/high-load-10m.json --window-minutes 10
```

The high-load metric is the sum of `uplinkBytes + downlinkBytes` for network
operations fully contained in the candidate window. Boundary events are
aggregated in a temporary SQLite database, so memory does not grow with the
number of operations. Equal-load windows choose the earliest start.

The writer:

- keeps only tasks with at least one contained network operation;
- keeps fully contained local operations in those tasks;
- removes empty traces/tasks;
- rebases operation and task arrival offsets;
- removes dependency IDs pointing to omitted operations;
- preserves root/trace/task/operation metadata;
- atomically replaces the output file;
- removes its temporary SQLite/output files on success or failure.

## Testing and debugging

### Module tests

```bash
./test.py -s llm
```

Covered behavior includes trace parsing, both distribution policies, schedule
ordering, `AppTxTag`, readiness timing and fatal invariants, typed TOML/CLI
configuration, safe run paths, topology choices, and statistics
attribution/JSON.

### Registered example smoke test

The example requires `--config`, so select its registered parameterized entry
with a wildcard:

```bash
./test.py -e 'llm_sample*'
```

Running `./test.py -e llm_sample` without `*` invokes the executable without
the registered arguments; it reports the missing configuration and prints
usage.

### Streaming-script tests

```bash
cd contrib/llm
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest scripts/test_trace_stream.py
```

These tests cover streaming validation, numeric bounds, metadata preservation,
window boundaries, SQLite cleanup, atomic output, CLI behavior, and RAR
subprocess stderr handling.

### Logging

The `[logging]` section configures these components independently:

- `SampleScenario` at INFO;
- `ContentionAwareAgentDistribution` at INFO;
- `APGenerator`, `StaLlmGenerator`, and `TrafficSink` at WARN.

For debugger use:

```bash
./ns3 run "llm_sample --config contrib/llm/config/basic_config.toml" \
  --command-template="gdb --args %s"
```

Pass the normal program arguments through the `./ns3 run` command when using
the debugger.

## Project structure

```text
contrib/llm/
|-- CMakeLists.txt                    ns-3 module and test registration
|-- README.md                         English documentation
|-- README_RU.md                      Russian documentation
|-- config/basic_config.toml          Complete documented scenario configuration
|-- examples/
|   |-- sample-scenario.cc            Main executable orchestration
|   |-- scenario-config*              TOML/CLI parsing, validation, and option registry
|   |-- scenario-run-path.cc          CWD-based collision-safe run paths
|   |-- scenario-topology.*           BSS/STA/IP/application construction
|   |-- traffic-coordinator.*         Global TCP readiness barrier
|   |-- experiment-statistics*        Unified APP/TCP/MAC/PHY collection and summaries
|   |-- experiment-window-output.h    Typed window/entity/category output hierarchy
|   `-- experiment-json.cc            Streaming root writer and collision-safe output
|-- model/
|   |-- agent-data.h                  Shared trace/distribution data
|   |-- trace-parser.*                JSON-to-agent parser
|   |-- agent-distribution.*          Original distribution algorithm
|   |-- contention-aware-*            BSS and STA placement phases
|   |-- traffic-schedule.*            Pure UL/DL schedule construction
|   |-- app-tx-tag.*                  Cross-layer byte metadata
|   |-- ap-generator*                 AP downlink sender and statistics traces
|   |-- sta-llm-generator*            STA uplink sender and statistics traces
|   `-- traffic-sink.*                TCP receiver
|-- scripts/
|   |-- find_window.py                Streaming CLI
|   |-- live_test_traces.py           Exact four-trace live verification
|   |-- trace_stream.py               Parser, validator, selector, writer
|   `-- test_trace_stream.py          Python tests
|-- test/
|   |-- data/minimal-trace.json       Small deterministic fixture
|   |-- examples-to-run.py            Registered example invocation
|   `-- *-test-suite.cc               C++ unit/characterization tests
|-- traces/                            Git LFS datasets and derived trace
|-- lib/json.hpp                       Vendored nlohmann JSON header
|-- lib/toml.hpp                       Vendored toml++ 3.4.0 single header
`-- docs/superpowers/                  Refactor/streaming design history
```

## Glossary

| Term | Meaning in this project |
|---|---|
| Agent | Application-level LLM workload identity, not an ns-3 node |
| Operation | One trace step with timing and optional bidirectional bytes |
| Task | Container of related operations for one agent |
| Trace epoch | Common simulation time corresponding to trace offset zero |
| AP | IEEE 802.11 access point; one per BSS |
| STA | Physical non-AP Wi-Fi station; may host many agents |
| BSS | One AP, its associated STAs, SSID, and channel object |
| UL / uplink | STA-to-AP traffic, sent at `startOffsetMs` |
| DL / downlink | AP-to-STA traffic, sent at `startOffsetMs + durationMs` |
| MAC | Wi-Fi medium-access-control layer |
| PHY | Wi-Fi physical-layer abstraction |
| MPDU | MAC protocol data unit; a MAC frame |
| PSDU | PHY service data unit; one or more MPDUs presented to PHY |
| PPDU | PHY protocol data unit transmitted over the channel |
| A-MPDU | Aggregated collection of MPDUs in one PHY transmission |
| TXVECTOR | PHY transmission parameters, including mode/rate |
| CCA_BUSY | PHY reports that the primary channel is busy |
| RTT | TCP round-trip-time sample |
| AppTxTag | Byte tag connecting application writes to PHY observations |
| Tagged bytes | Application payload range carrying `AppTxTag` |
| Unique bytes | Tagged bytes counted only on their first observed attempt |
| Airtime | Modeled PPDU transmission duration, allocated by tagged bytes |
| Sparse window | A configured-width bucket emitted only when tagged traffic exists |

## Known limitations

- The C++ parser loads the entire JSON document into memory. Use sliced JSON,
  not multi-gigabyte source documents.
- Only operations with both byte directions positive are replayed.
- The trace's LLM/token/model metadata is not used in network behavior.
- BSSs use physically isolated channel objects by default. Shared-channel
  mode models common-medium interaction but remains unsuitable for detailed
  OBSS spatial-reuse studies.
- The scenario does not model a detailed building/campus radio environment.
- It does not explicitly configure OFDMA, MU-MIMO, beamforming, TWT,
  BSS coloring, OBSS-PD, authentication, or encryption.
- PHY payload measurements include retransmissions; use
  `phy_stats.*.unique_tagged_payload_bytes` when deduplicated payload is
  required.
- The device-level `packet size - 60` flow diagnostic is approximate.
- TX/RX timestamp matching uses a tuple and observation order; it is a
  diagnostic, not a formal end-to-end flow monitor.
- Trace application duration and Wi-Fi/TCP delay are different quantities.
- Results can change across ns-3 versions because Wi-Fi and rate-control
  models continue to evolve.

For the definitive model behavior, read the implementation and the official
ns-3 Wi-Fi model documentation linked above.
