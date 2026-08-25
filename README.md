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
  -> write one experiment JSON with Wi-Fi, transmission, cross-layer, and config data
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
| Statistics window | 10 ms | Sparse PHY JSON buckets |
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
3. tagged bytes are added to the configured PHY statistics window;
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

The experiment writes one JSON document. Its integer `schema_version` is `1`.
This is an intentional breaking schema: consumers of output from an older
version must be updated to the names and structure below.

`general.output_name` selects the filename inside the run folder and defaults
to `output.json`. The writer creates that file exclusively, so an existing
file is never truncated or overwritten. End-of-experiment transmission and
cross-layer summaries are present only in this JSON; they are not printed to
stdout or ns-3 logs. Ordinary startup, progress, error, and completion
messages remain.

### Root schema

The root members are:

| Field | Meaning |
|---|---|
| `schema_version` | Integer schema version, currently `1` |
| `measurement_semantics` | Descriptions of the tagged MAC/PHY measurements |
| `statistics_window_ms` | Configured sparse Wi-Fi window width in milliseconds |
| `wifi_windows` | Sparse non-empty Wi-Fi windows |
| `wifi_summary` | Per-AP totals over the emitted Wi-Fi windows |
| `transmission_summary` | Matched device TX/RX measurements by sender |
| `cross_layer_summary` | Per-node interval and whole-experiment measurements |
| `validation` | Wi-Fi payload consistency flags |
| `experiment_metadata` | Effective experiment configuration |

The document has this shape; object member order is not semantically
significant:

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

`experiment_metadata` is streamed last. Missing sparse Wi-Fi windows, access
points, or flows mean that no tagged payload was recorded for them.

### Measurement semantics

`measurement_semantics` contains:

| Field | Meaning |
|---|---|
| `mac_payload_source` | `PhyTxBegin+PhyTxPsduBegin/AppTxTag` |
| `mac_payload_byte_semantics` | Tagged application payload observed at PHY; retransmissions included |
| `phy_data_rate_semantics` | Airtime-weighted nominal `WifiTxVector` rate of actual tagged PPDU attempts; retransmissions included; PPDU airtime is allocated by tagged payload bytes |

### Wi-Fi windows and summary

Each item in `wifi_windows` has `window_end_ms` and an `access_points` array.
Each access-point entry has `access_point_id`, `uplink`, and `downlink`.
Each direction contains `total_payload_bytes` and a `flows` array. A window
flow contains:

| Field | Unit | Meaning |
|---|---:|---|
| `station_ipv4` | IPv4 address | STA source for uplink or destination for downlink |
| `payload_bytes` | bytes | Tagged application payload observed at PHY, including repeated attempts |
| `throughput_mbps` | Mbps | Payload rate over the configured window |
| `average_phy_data_rate_mbps` | Mbps or `null` | Airtime-weighted nominal PHY data rate; `null` when no PHY attempt exists |
| `phy_transmission_attempt_count` | count | PPDU attempts representing this flow and direction |
| `phy_transmission_airtime_us` | us | Allocated share of PPDU airtime |

The window rate uses SI Mbps directly:

```text
throughput_mbps = payload_bytes * 8 / statistics_window_us
```

For example, `125000 * 8 / 10000 = 100` Mbps in a 10 ms window. The result
includes retransmitted tagged bytes and is not unique application goodput.

`wifi_summary` has one object per registered AP, with the same
`access_point_id`, `uplink`, and `downlink` shape. Each direction again has
`total_payload_bytes` and `flows`. A summary flow contains
`station_ipv4`, `total_payload_bytes`, `average_phy_data_rate_mbps`,
`phy_transmission_attempt_count`, and `phy_transmission_airtime_us`. It has no
whole-experiment throughput field because that measurement is not defined by
the Wi-Fi summary.

### Transmission summary

`transmission_summary.senders` contains every sender with a recorded MAC
transmit payload sample, ordered by IPv4 string. Each entry contains:

| Field | Unit | Meaning |
|---|---:|---|
| `sender_ipv4` | IPv4 address | Packet source |
| `matched_packet_count` | count | TX/RX pairs with a strictly positive duration |
| `total_transmission_duration_us` | us | Sum of positive matched RX-minus-TX durations |
| `transmitted_payload_bytes` | bytes | MAC transmit sample total, including repeated attempts |
| `effective_throughput_mbps` | Mbps or `null` | Effective rate over positive matched duration |

Matching uses source and destination IPv4 addresses, TCP ports, estimated
payload size, and observation order. The device-level payload estimate is
`packet size - 60` after checking LLC/SNAP, IPv4, and TCP headers, so this is a
diagnostic match rather than an ns-3 `FlowMonitor` delay metric. The rate uses
SI Mbps:

```text
effective_throughput_mbps = transmitted_payload_bytes * 8 /
                            total_transmission_duration_us
```

Bytes times eight divided by microseconds is Mbps. When no positive matched
duration exists, the count and duration are zero and
`effective_throughput_mbps` is `null`.

### Cross-layer summary

`cross_layer_summary.nodes` contains every registered node, including nodes
with no measurements. A node has `node_id`, `node_label`,
`one_second_intervals`, and `overall`. All-zero interval and overall objects
are retained. The final interval can have `interval_duration_s` below one;
rates and utilization use that actual duration.

Each `one_second_intervals` entry contains:

| Field | Unit | Meaning |
|---|---:|---|
| `interval_index` | index | Zero-based one-second bucket |
| `interval_start_s` | s | Start relative to the experiment epoch |
| `interval_duration_s` | s | Actual interval duration |
| `application_to_phy_delay` | object | First-transmission tagged delay distribution |
| `application_transmit_throughput_mbps` | Mbps | TCP socket-accepted application payload rate |
| `phy_payload_throughput_mbps` | Mbps | Tagged PHY payload rate, including retransmissions |
| `unique_phy_payload_throughput_mbps` | Mbps | Deduplicated tagged PHY payload rate |
| `channel_utilization_percent` | percent | TX, RX, and CCA_BUSY time, capped at 100 percent |
| `phy_retransmission_count` | count | Repeated tagged MPDU identities |
| `mac_transmit_drop_count` | count | MAC transmit-drop events |
| `mac_transmit_drop_bytes` | bytes | Payload bytes in MAC transmit drops |
| `mac_mpdu_drop_count` | count | Dropped MPDUs |
| `mac_mpdu_drop_bytes` | bytes | Bytes in dropped MPDUs |
| `mac_data_failure_count` | count | MAC data-failure events |
| `mac_final_data_failure_count` | count | Final MAC data-failure events |
| `application_drop_event_count` | count | Application send-drop events |
| `application_drop_bytes` | bytes | Application payload rejected by TCP send |
| `mac_mpdu_drops_by_reason` | array | MPDU drop counts grouped by reason code |
| `application_drops_by_agent` | array | Application drop events and bytes grouped by agent |

The interval rate and utilization formulas are:

```text
throughput_mbps = payload_bytes * 8 / 1e6 / interval_duration_s
channel_utilization_percent = min(100, busy_time_us /
                                  (interval_duration_s * 1e6) * 100)
```

`application_to_phy_delay` contains `sample_count`, `mean_us`,
`standard_deviation_us`, `minimum_us`, and `maximum_us`. The standard deviation
is the population standard deviation. All four delay values are `0.0` when
`sample_count` is zero.

The drop breakdown objects are:

```json
{"reason_code": 7, "drop_count": 3}
```

```json
{"agent_key": "agent-1", "drop_event_count": 2, "dropped_payload_bytes": 4096}
```

The `overall` object contains:

| Field | Unit | Meaning |
|---|---:|---|
| `experiment_duration_s` | s | Whole experiment duration |
| `application_to_phy_delay` | object | Delay distribution merged across intervals |
| `application_transmitted_payload_bytes` | bytes | TCP socket-accepted application payload |
| `phy_payload_bytes` | bytes | Tagged PHY payload, including retransmissions |
| `unique_phy_payload_bytes` | bytes | Deduplicated tagged PHY payload |
| `phy_mpdu_bytes` | bytes | Complete tagged PHY MPDU bytes |
| `average_application_transmit_throughput_mbps` | Mbps | Application payload average over experiment duration |
| `average_phy_payload_throughput_mbps` | Mbps | PHY payload average over experiment duration |
| `average_channel_utilization_percent` | percent | Busy-time average over experiment duration, capped at 100 percent |
| `phy_retransmission_count` | count | Repeated tagged MPDU identities |
| `mac_transmit_drop_count` | count | MAC transmit-drop events |
| `mac_transmit_drop_bytes` | bytes | Payload bytes in MAC transmit drops |
| `mac_mpdu_drop_count` | count | Dropped MPDUs |
| `mac_mpdu_drop_bytes` | bytes | Bytes in dropped MPDUs |
| `mac_data_failure_count` | count | MAC data-failure events |
| `mac_final_data_failure_count` | count | Final MAC data-failure events |
| `application_drop_event_count` | count | Application send-drop events |
| `application_drop_bytes` | bytes | Application payload rejected by TCP send |
| `mac_mpdu_drops_by_reason` | array | Whole-experiment MPDU drop counts by reason code |

The two average throughput fields and average utilization use the same
formulas above with `experiment_duration_s` as the denominator. The per-agent
breakdown is an interval field; the overall object contains the aggregate
application drop count and bytes.

### Validation and experiment metadata

`validation.window_payload_totals_consistent` checks each emitted window's
flow-byte sum against its sparse total.
`validation.summary_payload_totals_consistent` checks the Wi-Fi summary totals
against the sparse PHY state.

`experiment_metadata.configuration` records all 36 effective values after:

```text
compiled defaults < TOML values < CLI overrides
```

The eight configuration objects and their fields are:

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

Values keep their TOML names and JSON scalar types. An omitted
`general.run_folder` is `null`, and enums use their canonical TOML spelling.
Path values are recorded exactly as configured: metadata does not contain
resolved config, trace, run-folder, or output paths, nor per-value provenance.

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
| `window_ms` | `--statistics-window-ms` | `10` ms sparse PHY window |

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
|   |-- traffic-flow-monitor.*        Device TX/RX matching and transmission summary
|   `-- wifi-statistics*              PHY/MAC collection and experiment JSON summaries
|-- model/
|   |-- agent-data.h                  Shared trace/distribution data
|   |-- trace-parser.*                JSON-to-agent parser
|   |-- agent-distribution.*          Original distribution algorithm
|   |-- contention-aware-*            BSS and STA placement phases
|   |-- traffic-schedule.*            Pure UL/DL schedule construction
|   |-- app-tx-tag.*                  Cross-layer byte metadata
|   |-- ap-generator*                 AP downlink sender and reports
|   |-- sta-llm-generator*            STA uplink sender and reports
|   `-- traffic-sink.*                TCP receiver
|-- scripts/
|   |-- find_window.py                Streaming CLI
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
  `cross_layer_summary` unique-payload fields when deduplicated payload is
  required.
- The device-level `packet size - 60` flow diagnostic is approximate.
- TX/RX timestamp matching uses a tuple and observation order; it is a
  diagnostic, not a formal end-to-end flow monitor.
- Trace application duration and Wi-Fi/TCP delay are different quantities.
- Results can change across ns-3 versions because Wi-Fi and rate-control
  models continue to evolve.

For the definitive model behavior, read the implementation and the official
ns-3 Wi-Fi model documentation linked above.
