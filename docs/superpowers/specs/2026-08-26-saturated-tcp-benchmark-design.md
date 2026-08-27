# Saturated TCP Wi-Fi Benchmark Design

> **Superseded:** This historical v1 design is superseded by
> [Saturated Data PHY Benchmark v2 Design](2026-08-27-saturated-data-phy-benchmark-v2-design.md).
> It is retained unchanged below as design history.

## Purpose

Add an independent, deterministic 802.11ax calibration benchmark to the
`llm` contrib module. The benchmark measures only Wi-Fi traffic transmitted
by stations under controlled RSSI, contention, traffic-direction, and MIMO
conditions. It does not replay LLM traces and does not use the existing agent
simulation or agent-distribution system.

The benchmark produces one normal experiment `output.json` per run and one
semicolon-separated `results.csv` across the complete experiment matrix. The
JSON retains detailed 10 ms windows; the CSV contains only the agreed station
and station-derived BSS metrics.

## Primary deliverables

- `examples/saturated-tcp-scenario.cc`: one directly runnable ns-3 example.
- `examples/saturated-tcp/`: focused C++ helpers for configuration, topology,
  traffic, station instrumentation, metrics, and output integration.
- `config/saturated_tcp_config.toml`: complete benchmark configuration.
- `exp_scripts/saturated_tcp_experiment.py`: sequential full-matrix runner.
- Focused C++ and Python tests.
- Shared JSON-schema extensions for station-side PHY metrics.
- A full honest matrix run under the outer ns-3 `run/` directory, followed by
  structural, mathematical, and trend analysis.

## Non-goals

- No LLM trace input, parser, agent generator, or agent distribution.
- No TCP-goodput metric in the required CSV or primary benchmark conclusions.
- No artificial PHY-rate, MCS, SIR, SINR, or minimum-SIR override.
- No custom OBSS-PD thresholds or spatial-reuse heuristics.
- No AP-originated traffic in station or BSS metric calculations.
- No averaging across repetition attempts.
- No parallel ns-3 processes.
- No second CSV or XLSX output.

TCP remains the saturated traffic source. The reported efficiency is PHY/MAC
station-transmission efficiency, not TCP application efficiency.

## Component architecture

```text
examples/saturated-tcp-scenario.cc
  -> saturated-tcp/config
  -> saturated-tcp/topology
       -> filtered native propagation
  -> saturated-tcp/traffic
       -> readiness barrier + SaturatedTcpSender
  -> saturated-tcp/sta-metrics
       -> PPDU rate/airtime collection
       -> EDCA channel-access waiting
  -> shared statistics DTOs/schema/JSON writer

exp_scripts/saturated_tcp_experiment.py
  -> saturated_tcp_config.toml
  -> sequential ns-3 subprocesses
  -> retained output.json files
  -> one results.csv
```

The benchmark helpers may reuse generic experiment identities, window
splitting, output DTOs, validation conventions, and the streaming JSON writer.
They must not depend on `APGenerator`, `StaLlmGenerator`, trace schedules,
trace parsing, or contention-aware agent distribution.

Suggested focused C++ units are:

```text
examples/saturated-tcp/
|-- config.{cc,h}
|-- config-internal.h
|-- topology.{cc,h}
|-- bss-link-filter.{cc,h}
|-- saturated-tcp-sender.{cc,h}
|-- traffic.{cc,h}
|-- readiness-barrier.{cc,h}
|-- access-tracking-sta-wifi-mac.{cc,h}
|-- sta-metrics.{cc,h}
|-- sta-metrics-internal.h
`-- output.{cc,h}
```

File boundaries may be adjusted during planning to follow existing CMake and
test seams, but responsibilities must remain separate and no monolithic helper
may absorb unrelated configuration, topology, traffic, and statistics logic.

## One-run configuration

`saturated_tcp_config.toml` contains only benchmark-relevant values. It has no
trace or agent-distribution settings.

```toml
[general]
output_name = "output.json"
# run_folder may be supplied by the matrix runner

[script]
repetitions = 1

[simulation]
rng_seed = 12345
rng_run = 1

[benchmark]
sta_count_per_bss = 5
rssi_range = "high"
interference_mode = "isolated"
traffic_mode = "ul"
mimo_mode = "su"

[wifi]
band = "5GHz"
channel_number = 42
bandwidth_mhz = 80
primary_20_index = 0
tx_power_dbm = 20.0
rate_manager = "ns3::MinstrelHtWifiManager"
antennas = 2
max_tx_spatial_streams = 2
max_rx_spatial_streams = 2

[tcp]
congestion_control = "ns3::TcpHighSpeed"
segment_size_bytes = 1460
send_buffer_bytes = 33554432
receive_buffer_bytes = 33554432
wired_rate = "10Gbps"
wired_delay = "0.1ms"

[statistics]
window_ms = 10

[logging]
scenario_level = "info"
```

The C++ executable runs one matrix configuration and one repetition attempt.
Every matrix-selecting TOML value has a clear CLI override for the Python
runner. `script.repetitions` is accepted as metadata but the C++ executable
does not loop over it.

Validated values are:

- `sta_count_per_bss`: 5, 10, 15, 20, 25, or 30 when launched by the matrix
  runner; direct runs reject values outside 1 through 30.
- `rssi_range`: `high`, `medium`, or `low`.
- `interference_mode`: `isolated` or `ap_only_cochannel`.
- `traffic_mode`: `ul`, `dl`, or `ul_dl`.
- `mimo_mode`: `su`, plus `mu` only if meaningful end-to-end support is
  verified.
- `script.repetitions`: positive integer, default 1.
- `statistics.window_ms`: positive and must divide the one-second measurement
  interval exactly.

## Network topology

Every run creates exactly three BSS groups:

```text
dedicated wired server -- 10 Gbit/s, 0.1 ms -- AP -- Wi-Fi -- STAs
```

Each AP routes between its own non-bottleneck point-to-point server subnet and
its Wi-Fi subnet. No server or wired link is shared between BSSs. Routing is
configured before traffic readiness is evaluated.

All APs and STAs use:

- IEEE 802.11ax;
- 5 GHz channel 42;
- 80 MHz channel width, primary 20 index 0;
- fixed 20 dBm transmit power;
- `ns3::MinstrelHtWifiManager`;
- two antennas;
- up to two TX and two RX spatial streams;
- fixed positions during a run.

AP BSS colors are explicitly 1, 2, and 3. A color of zero is not used. Every
other BSS-color, spatial-reuse, and OBSS-PD setting retains its ns-3 default.

### Native propagation

Allowed links use the native deterministic Yans defaults:

- `YansWifiChannel`;
- `LogDistancePropagationLossModel`;
- `ConstantSpeedPropagationDelayModel`;
- no random fading model.

The benchmark fixes role/BSS visibility through one scenario-specific
propagation wrapper. The wrapper delegates allowed links to the native
LogDistance model and suppresses forbidden links. It does not alter received
power for allowed links.

Allowed visibility is:

| Sender/receiver relationship | Isolated mode | AP-only co-channel mode |
|---|---:|---:|
| Same BSS, any AP/STA pair | native | native |
| Different BSS, AP to AP | impossible (separate channels) | native |
| Different BSS, any link involving a STA | impossible | blocked |

Thus only AP-to-AP signals cross BSS boundaries in the interference mode.
This is an explicit programmatic benchmark assumption and must be documented
as such; it is not a claim about physical 802.11 propagation.

### RSSI-calibrated placement

Transmit power and the concrete native loss model are used to solve distance,
not to override RSSI, MCS, rate, or SINR.

Target desired-link RSSI is:

| Range | Target |
|---|---:|
| High | -41.5 dBm |
| Medium | -50.0 dBm |
| Low | -60.0 dBm |

The accepted tolerance is target +/- 0.5 dB.

APs are placed at the vertices of an equilateral triangle. Its side length is
solved so every AP-to-AP allowed link is -50.0 +/- 0.5 dBm. The same positions
are used in isolated mode, but separate channel objects make cross-BSS
visibility impossible.

Every STA in a BSS is placed at the same solved radius from its AP, evenly
spaced around a circle. BSS rings receive deterministic angular offsets to
avoid identical coordinates. Placement never changes between repetition
attempts. Repetitions alter ns-3 random streams only.

Before traffic starts, the scenario evaluates native received power for every
associated AP/STA link and each allowed AP/AP link. Any target outside tolerance
aborts the run with the BSS, node, target, actual RSSI, and distance.

## MIMO modes

In `su` mode, an AP may use up to two spatial streams with one 2x2 STA.

The checked-out ns-3 `MultiUserScheduler` explicitly states that DL MU-MIMO is
not yet supported. Its available scheduled UL multi-user path is OFDMA, not
MU-MIMO. Therefore this implementation exposes `su` only and rejects `mu`
with a precise unsupported-mode diagnostic.

If a later ns-3 version adds meaningful scheduled end-to-end MU-MIMO, a future
change may add `mu` as a separate mode with these semantics:

- AP has two total spatial streams;
- up to two STAs are served simultaneously;
- normally one stream is allocated per STA;
- UL MU is enabled only where the complete trigger/scheduler/PHY path exists;
- DL and UL behavior are not simulated through artificial concurrency.

The current runner prints and documents the limitation. It must not produce a
`mu` label for an SU or OFDMA-only result.

## Saturated TCP traffic

Each STA has one independent application-data flow per active direction:

- UL: STA saturated sender to its BSS's wired server sink.
- DL: wired server saturated sender to that STA sink.
- UL+DL: both independent TCP connections simultaneously on separate ports.

All three BSSs use the same traffic mode in a run.

The built-in `BulkSendApplication` cannot be used directly because it begins
sending as soon as its TCP connection succeeds, before a common readiness
barrier can open. A focused benchmark-only `SaturatedTcpSender` implements the
same unlimited send-buffer-filling behavior while separating connection
readiness from payload start:

- connect the TCP socket early;
- begin setup after a one-second association interval and stage flows by 10 ms
  in deterministic installation order;
- report ready exactly once when connection succeeds;
- after a socket exhausts its connection cohort, trace the failure and retry
  after one second with a fresh socket preserving Local, Remote, and TOS;
- send no payload before `StartTraffic()`;
- after `StartTraffic()`, keep calling `Socket::Send()` until the send buffer
  fills, then resume from the socket send callback;
- impose no byte or data-rate limit;
- stop cleanly after the measured second.

There is no application data-rate throttle. TCP and Wi-Fi determine achieved
traffic. TCP uses `ns3::TcpHighSpeed`, a 1460-byte segment payload, and 32 MiB
send/receive buffers.

### Event-driven measurement epoch

There is no fixed payload warm-up or fixed measurement start. Pre-measurement
connection setup follows the current `llm-scenario` readiness pattern:

1. Install sinks at time zero, then stage sender connection setup from 1.00 s
   in deterministic 10 ms increments.
2. Wait for every STA association and every active BulkSend connection.
3. Every `SaturatedTcpSender` reports ready without sending payload early.
4. Select the first whole-second boundary strictly after complete readiness.
5. Reset/start all metric collectors and all senders at that common epoch.
6. Measure exactly one second.
7. Stop applications, finalize statistics, validate output, and end the
   simulator immediately after the interval.

A 400-second conservative safety stop spans a complete fresh-socket
replacement cohort and turns missing association/connection readiness into a
clear failure rather than an infinite simulation. The safety stop is not a
warm-up timer and never starts measurement early.

## Station-only measurement semantics

All new benchmark metrics observe only PPDUs transmitted by each STA. AP
transmissions are never directly included. The same rule applies in UL, DL,
and UL+DL:

- UL station traffic includes data, acknowledgements, control, and retries
  transmitted by the STA.
- DL station traffic normally consists of TCP ACK data frames, MAC ACK or
  BlockAck, control frames, and retries transmitted by the STA.
- UL+DL includes every qualifying STA transmission from both connections.

Relevant station-transmitted PPDUs include:

- TCP data frames;
- TCP ACK frames;
- MAC ACK and BlockAck;
- RTS/CTS or other traffic-related control frames;
- aggregated MPDUs;
- every retransmission attempt.

Exclude beacon, association/probe, broadcast, and unrelated traffic. These
events occur outside the measured epoch where possible and are not attributed
to one station metric.

### Raw PPDU accumulators

For each station and statistics window, retain:

- sum of complete qualifying PPDU airtime;
- sum of nominal rate multiplied by PPDU airtime;
- sum of PSDU bytes carried in qualifying PPDUs;
- EDCA waiting intervals, tracked as a union rather than per-frame sums.

Every nominal rate comes from the actual `WifiTxVector`:

- HE data uses its selected MCS, channel width, guard interval, and NSS;
- control/legacy PPDUs use their actual selected mode/rate.

No fixed MCS is inferred for control traffic.

### Derived station metrics

For a station with positive PPDU airtime:

```text
average_theoretical_phy_rate_mbps =
  sum(nominal_rate_bps * ppdu_airtime_us)
  / sum(ppdu_airtime_us)
  / 1,000,000
```

```text
average_practical_phy_rate_mbps =
  sum(psdu_bytes * 8)
  / sum(ppdu_airtime_seconds)
  / 1,000,000
```

Complete PPDU duration includes the PHY preamble, PHY headers, and payload
transmission time. PSDU bytes include MAC/TCP/IP contents and repeated copies
when retransmitted. TCP application goodput is not used.

```text
channel_efficiency =
  average_practical_phy_rate_mbps
  / average_theoretical_phy_rate_mbps
```

### EDCA contention measurement

For each STA, contention waiting is the wall-clock union of intervals during
which at least one STA EDCA frame is waiting for channel access. It includes:

- AIFS waiting;
- backoff countdown;
- frozen backoff while the medium is busy.

It excludes:

- STA PHY TX and RX time outside a pending access interval;
- time with no frame waiting;
- MAC ACK sent after SIFS without ordinary EDCA contention.

Multiple pending access categories are combined as an interval union so the
fraction cannot exceed one merely because two queues wait concurrently.

```text
contention_fraction =
  union_edca_waiting_time
  / statistics_window_duration
```

A scenario-specific STA MAC subclass observes the existing
`WifiMac::NotifyRequestAccess` seam and per-AC TXOP-start traces. It retains the
standard STA MAC behavior and only emits measurement callbacks. No core Wi-Fi
source modification is required.

Waiting and PPDU intervals are split exactly across 10 ms window boundaries.
The `overall` result is rebuilt from raw accumulators, not by averaging already
rounded window JSON values.

## BSS aggregation

The AP object represents a BSS aggregate derived exclusively from its station
results. For N stations:

```text
avg_all_sta_theoretical_phy_rate_mbps =
  mean(defined sta_i_average_theoretical_phy_rate_mbps values)
```

```text
avg_all_sta_practical_phy_rate_mbps =
  mean(defined sta_i_average_practical_phy_rate_mbps values)
```

```text
bss_channel_efficiency =
  avg_all_sta_practical_phy_rate_mbps
  / avg_all_sta_theoretical_phy_rate_mbps
```

```text
bss_channel_contention_fraction =
  mean(sta_i_contention_fraction)
```

No AP-originated PPDU or AP contention value enters these calculations. A
station with no qualifying PPDU in a short window has null rate/efficiency
values and is excluded from rate means for that window. The dense `overall`
array still contains every configured station. A station with zero qualifying
PPDU over the exact measured second has null theoretical rate, practical rate,
and efficiency plus numeric contention (including numeric zero), and is
excluded from overall BSS rate means. If a BSS has no defined station rate,
its theoretical rate, practical rate, and efficiency are null. BSS contention
always averages every configured station's numeric contention.

## Shared JSON schema

The benchmark reuses the existing root ordering and hierarchy:

```text
schema_version
measurement_semantics
statistics_window_ms
windows
overall
validation
experiment_metadata
```

AP objects remain BSS aggregates and station objects remain individual STA
detail. Configuration metadata is benchmark-specific but stays below
`experiment_metadata.configuration`. Entity inventory and ordering conventions
remain unchanged.

Add these non-directional fields at the top of each entity's `phy_stats`:

```text
average_theoretical_phy_rate_mbps
average_practical_phy_rate_mbps
channel_efficiency
contention_fraction
```

For a benchmark station, fields hold that STA's values. For a benchmark AP,
they hold the station-derived BSS values. In ordinary `llm-scenario` output,
these new optional fields are null unless the corresponding station-side
collector is connected. Existing directional and non-directional fields retain
their current meaning.

The fields appear in every sparse window entity and in `overall`. Undefined
rate/efficiency values are JSON null. Contention is zero when no access wait is
observed. The shared JSON remains two-space-indented and streaming.

The existing validation object retains its current keys. Before writing,
benchmark-specific validation rejects:

- non-finite or negative rate values;
- efficiency or contention outside [0, 1], allowing only a documented small
  floating-point tolerance at the boundary;
- practical rate materially greater than theoretical rate;
- AP/BSS fields that do not reproduce the station formulas;
- partial null rate triplets, nonnumeric contention, or overall rate presence
  inconsistent with sparse-window PPDU observations;
- inventory/configuration mismatches.

## Full experiment matrix

The Python runner uses this fixed order:

```text
sta_count_per_bss: 5, 10, 15, 20, 25, 30
rssi_range: high, medium, low
interference_mode: isolated, ap_only_cochannel
traffic_mode: ul, dl, ul_dl
mimo_mode: su
repetition_attempt: 1 through script.repetitions
```

`experiment_id` is a stable sequential integer for one matrix configuration
(STA count + RSSI + interference + traffic + MIMO). It is shared by every BSS
and repetition of that configuration. A CSV row is uniquely identified by:

```text
experiment_id + repetition_attempt + bss_id
```

The runner fixes:

```text
rng_seed = 12345
rng_run = repetition_attempt
```

The same attempt number is used across all configurations for controlled,
reproducible comparisons. Repetition attempts are separate rows and are never
averaged.

With SU only and the default one repetition:

```text
6 * 3 * 2 * 3 * 1 = 108 ns-3 runs
108 * 3 BSS rows = 324 CSV rows
```

The current ns-3 capability does not add a second MIMO mode, so the honest
default matrix is exactly 108 runs and 324 rows.

## Runner behavior and retained files

The runner is invoked from the outer ns-3 root:

```bash
python3 contrib/llm/exp_scripts/saturated_tcp_experiment.py
```

It creates:

```text
run/scripted_exp_<timestamp>/
|-- results.csv
|-- experiment_001/
|   `-- attempt_1/output.json
|-- experiment_002/
|   `-- attempt_1/output.json
`-- ...
```

It runs exactly one ns-3 subprocess at a time, in fixed matrix order. It
validates each JSON before appending exactly three BSS rows. A nonzero process
status, unsupported requested mode, missing output, schema error, wrong
configuration, wrong inventory, or invalid metric stops immediately. Completed
JSON and CSV rows remain. The failed attempt contributes no CSV row. Existing
directories/files are never overwritten.

## CSV contract

There is exactly one CSV. It is optimized for direct Microsoft Excel opening:

- field separator: semicolon (`;`);
- encoding: UTF-8 with BOM;
- line endings: CRLF;
- decimal separator: dot;
- standard double-quote escaping;
- fixed header through station index 29.

One row represents one BSS in one repetition attempt. Columns are exactly:

```text
experiment_id
repetition_attempt
sta_count_per_bss
rssi_range
target_rssi_dbm
interference_mode
traffic_mode
mimo_mode
bss_id

avg_all_sta_theoretical_phy_rate_mbps
avg_all_sta_practical_phy_rate_mbps
bss_channel_efficiency
bss_channel_contention_fraction

sta_0_avg_theoretical_phy_rate_mbps
sta_0_avg_practical_phy_rate_mbps
sta_0_efficiency
sta_0_contention_fraction

...

sta_29_avg_theoretical_phy_rate_mbps
sta_29_avg_practical_phy_rate_mbps
sta_29_efficiency
sta_29_contention_fraction
```

No additional diagnostic columns are added. Station columns at indexes greater
than or equal to `sta_count_per_bss` are all empty. For an existing station
with undefined overall rates, its theoretical, practical, and efficiency cells
are empty while contention stays numeric, including `0.0`. The three BSS
rate/efficiency cells follow the same rule when no station rate is defined;
BSS contention remains numeric. Existing retransmission, failure, drop,
busy-time, and airtime diagnostics remain available only in the retained JSON
files.

## Failure and safety behavior

- Configuration errors name TOML path, CLI flag, rejected value, and expected
  constraint.
- Association/readiness safety timeout is a failure, never an implicit traffic
  start.
- RSSI placement failures include target, actual, distance, node, and BSS.
- Metrics fail before JSON/CSV publication if non-finite or inconsistent.
- JSON uses exclusive creation and existing write/flush/close error handling.
- CSV is initialized exclusively; complete attempt rows are appended together
  only after all three BSS records validate.
- SIGINT/subprocess failure leaves completed evidence and exits nonzero.
- The runner never deletes retained successful JSON or the CSV.

## Testing strategy

### C++ tests

Focused tests cover:

- TOML/CLI parsing and validation;
- enum spellings and configuration metadata;
- distance solving and RSSI tolerance;
- equilateral AP and deterministic ring coordinates;
- every same/cross-BSS role pair in isolated/shared link filtering;
- 2x2 PHY attributes and BSS colors 1/2/3;
- sender connect/ready/start/send-buffer behavior and flow construction for
  UL, DL, and UL+DL;
- readiness barrier, next-second epoch, one-second stop, and timeout failure;
- all relevant PPDU kinds, TxVector rates, PSDU bytes, PPDU airtime, and retries;
- access request/grant intervals, frozen backoff, multiple-AC union, and window
  splitting;
- station formulas, nullable short windows and overall values, overall raw
  merge, defined-only BSS rate means, and all-station BSS contention;
- shared JSON field placement/order/nulls and unchanged ordinary scenario
  output;
- output no-clobber and error paths.

### Python tests

Focused tests cover:

- exact matrix ordering and count;
- stable experiment IDs;
- attempt-to-RNG mapping;
- SU-only mode list and precise rejection of unsupported `mu`;
- exact ns-3 command construction;
- sequential fail-fast execution;
- retained directory paths;
- JSON validation and exact three-row append;
- no append for failed attempts;
- exact 133-column CSV header, unused station blanks, nullable existing-rate
  blanks, and numeric-zero contention cells;
- semicolon, UTF-8 BOM, CRLF, quoting, and decimal-dot output.

All existing llm C++, Python, live-verifier, and registered example tests must
remain green.

## Full honest run and discrepancy audit

After implementation and deterministic tests, run the complete default matrix
from `/home/bsa/projects/ns-3-dev`. No reduced duration, generated result, or
test-only shortcut is permitted. Retain the final successful run directory.

Audit every JSON and CSV row for:

- exact matrix, attempt, BSS, and station coverage;
- exact configuration metadata;
- target RSSI tolerance;
- finite nonnegative values;
- practical rate not materially above theoretical rate;
- exact station and BSS efficiency formulas;
- contention in [0, 1];
- exact BSS means from station columns;
- empty columns only for nonexistent STAs;
- ordinary JSON schema shape and field consistency.

Analyze, but do not force, these empirical hypotheses:

- lower target RSSI generally lowers nominal/practical station rates;
- increased STA count generally increases station contention;
- AP-only co-channel mode changes AP carrier sensing and therefore station
  traffic behavior relative to isolated BSSs;
- Minstrel probing may create non-monotonic individual results;
- the three BSSs may differ despite symmetric geometry because MAC/TCP random
  streams differ.

Trend violations are warnings requiring inspection, not automatic failures.
Structural, formula, range, missing-data, or metadata discrepancies are
implementation failures. If a defect is fixed, rerun the full matrix to
produce one final accepted dataset.

The handoff reports the retained run path, supported MIMO modes, exact run/row
counts, wall time, deterministic test results, discrepancy checks, and every
suspicious trend or outlier found.
