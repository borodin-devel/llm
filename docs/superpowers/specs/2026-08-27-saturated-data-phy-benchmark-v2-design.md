# Saturated Data PHY Benchmark v2 Design

## Purpose

Replace the saturated benchmark's blended data/control PHY metrics with a
data-only model that answers three separate questions:

1. Which HE channel-width/NSS/MCS profile carried most station-transmitted data bytes?
2. How efficiently were transmitted data bytes packed into the station's own
   data PPDU airtime?
3. How much transmitted-data rate remained when the denominator was the full
   one-second measurement interval?

The matrix gains a matching one-STA-per-BSS baseline. The external runner uses
that baseline to report signed aggregate gross transmitted-data rate change.
It also becomes resource-aware and parallel while preserving deterministic,
single-owner CSV publication.

This is an incompatible schema replacement. Backward compatibility is not a
goal.

## Fixed scenario contract

The independent `saturated-tcp-scenario` retains:

- three BSSs with one wired server and one AP per BSS;
- IEEE 802.11ax in the 5 GHz band, channel 42, 80 MHz, primary 20 index zero;
- 20 dBm fixed transmit power;
- `ns3::MinstrelHtWifiManager`;
- two antennas and up to two transmit and receive spatial streams on APs and
  STAs;
- SU operation only;
- `ns3::TcpHighSpeed`, 1460-byte segments, 32 MiB TCP buffers, and dedicated
  10 Gbps/0.1 ms wired backhaul;
- the native RSSI placement and the existing isolated/AP-only cochannel
  propagation policies;
- a readiness-selected whole-second epoch and exactly one measured second;
- 10 ms statistics windows by default;
- repetitions configurable with a default of one.

The Wi-Fi configuration additionally fixes:

```text
HE guard interval: 3200 ns
RTS/CTS threshold: 0 bytes
```

Both values are explicit TOML/CLI/configuration-metadata fields. The operating
channel remains 80 MHz. A qualifying data `WifiTxVector` must be HE SU with a
3200 ns guard interval and an actual Minstrel-selected width of 20, 40, or 80
MHz. Wider or otherwise invalid widths are scenario errors.

## Data-only transmission scope

Metrics observe `PhyTxPsduBegin` on each STA PHY. A qualifying MPDU:

- has a data header;
- has the registered STA as transmitter;
- has a unicast receiver.

This includes:

- saturated uplink TCP data;
- TCP ACK packets carried in Wi-Fi data frames in DL and UL+DL;
- every attempted retransmission of a qualifying data MPDU;
- all qualifying subframes in a data A-MPDU.

It excludes:

- MAC ACK and BlockAck;
- RTS and CTS;
- management frames;
- broadcast/group frames;
- AP-transmitted data and control frames.

The byte unit is transmitted data PSDU/MAC bytes. A complete qualifying PSDU
uses its PSDU size. A qualifying A-MPDU retains subframe delimiters and padding
through `GetAmpduSubframeSize`. PHY preamble and PHY-header bytes are not part
of the byte count. Retransmissions count the repeated bytes again because the
metrics intentionally describe gross attempted transmission volume, not
unique payload or goodput.

## Raw channel-width/NSS/MCS profiles

The profile key is:

```text
(channel_width_mhz, nss, mcs)
```

HE SU and guard interval are fixed scenario invariants. Actual data TxVector
width is part of the profile because Minstrel selects among widths permitted by
the 80 MHz operating channel. Each station/window key accumulates:

```text
transmitted_psdu_bytes
ppdu_attempt_count
ppdu_airtime_ns
```

One SU data PPDU contributes to one width/NSS/MCS key. Its complete PPDU airtime
comes from `WifiPhy::CalculateTxDuration`. Bytes and airtime are split
proportionally when the PPDU crosses a 10 ms window or the one-second endpoint.
The attempt count belongs to the window containing the PPDU start and appears
exactly once in `overall`.

Profiles serialize in ascending `(channel_width_mhz, nss, mcs)` order. The
dominant profile is selected by:

1. greatest attributed bytes;
2. greatest nominal `GetDataRate` on an exact byte tie;
3. ascending `(channel_width_mhz, nss, mcs)` as the final deterministic
   tie-break.

## Station-derived fields

For positive attributed data bytes and data PPDU airtime:

```text
dominant_data_phy_rate_mbps =
  GetDataRate(dominant width/NSS/MCS, GI 3200 ns)
  / 1,000,000
```

```text
dominant_data_profile_share =
  dominant_profile_bytes / total_profile_bytes
```

```text
effective_phy_rate_mbps =
  total_transmitted_data_psdu_bytes * 8
  / total_data_ppdu_airtime_seconds
  / 1,000,000
```

```text
data_tx_rate_over_interval_mbps =
  total_transmitted_data_psdu_bytes * 8
  / statistics_interval_seconds
  / 1,000,000
```

```text
data_tx_opportunity_gap_fraction =
  1 - data_tx_rate_over_interval_mbps / effective_phy_rate_mbps
```

The gap is algebraically one minus the station's qualifying data PPDU airtime
fraction. It is all measured time outside the station's own data PPDU airtime,
not a pure EDCA, NAV, collision, or backoff measurement.

For an existing STA with no qualifying data PPDU:

```text
dominant_data_phy_rate_mbps: null
dominant_data_profile_share: null
effective_phy_rate_mbps: null
data_tx_rate_over_interval_mbps: 0.0
data_tx_opportunity_gap_fraction: null
data_tx_profile: []
```

All rate and fraction values must be finite and non-negative. Shares are in
`(0, 1]`; gaps are in `[0, 1]` with only floating-point boundary tolerance.
The profile byte and airtime sums must reproduce the published fields.

## BSS aggregation

The AP output remains a BSS parent derived only from station values:

```text
mean_dominant_data_phy_rate_mbps =
  mean(defined station dominant rates)
```

```text
mean_effective_phy_rate_mbps =
  mean(defined station effective rates)
```

```text
aggregate_data_tx_rate_over_interval_mbps =
  sum(all configured station interval rates)
```

Undefined station rates are excluded from the two means. Every configured STA
contributes to the aggregate, including numeric zero. If every STA rate is
undefined, both means are null while the aggregate is numeric zero.

No AP PPDU enters these values. In DL, these remain station-side TCP-ACK data
metrics and must not be described as AP downlink capacity.

## JSON schema version 2

The root remains ordered as:

```text
schema_version
measurement_semantics
statistics_window_ms
windows
overall
validation
experiment_metadata
```

`schema_version` becomes `2` because the benchmark PHY fields are replaced.
Every `phy_stats` object contains the fixed keys:

```text
dominant_data_phy_rate_mbps
dominant_data_profile_share
effective_phy_rate_mbps
data_tx_rate_over_interval_mbps
data_tx_opportunity_gap_fraction
data_tx_profile
mean_dominant_data_phy_rate_mbps
mean_effective_phy_rate_mbps
aggregate_data_tx_rate_over_interval_mbps
```

For a saturated station, the first six fields are populated and the last
three are null. For a saturated AP/BSS, the first five are null,
`data_tx_profile` is an empty array, and the last three are populated. For
ordinary `llm-scenario` entities all eight numeric fields are null and the
profile is empty. Existing non-benchmark PHY fields retain their meanings.

Each profile entry is an ordered object:

```json
{
  "channel_width_mhz": 80,
  "nss": 2,
  "mcs": 11,
  "transmitted_psdu_bytes": 100500.0,
  "ppdu_attempt_count": 120,
  "ppdu_airtime_us": 900.0
}
```

The existing eight validation flags remain unchanged. Benchmark-specific
validation additionally enforces data-profile ordering, sums, derived
formulas, entity roles, dense overall inventory, and overall/window raw
reconstruction.

Windows remain sparse, but activity is now data-only: a station without a
qualifying data profile is absent from that window, an AP/BSS without an
active station is absent, and a completely inactive window is absent.
`overall` remains dense and contains every configured AP and STA.

The removed benchmark fields are:

```text
average_theoretical_phy_rate_mbps
average_practical_phy_rate_mbps
channel_efficiency
contention_fraction
```

## Removed access-wait implementation

The benchmark no longer publishes EDCA access-wait or backoff/NAV metrics.
Delete the scenario-specific `AccessTrackingStaWifiMac`, `AccessWaitTracker`,
their trace subscriptions, tests, and CMake entries. Install ordinary
`ns3::StaWifiMac` again.

Do not add replacements for:

```text
data_tx_airtime_fraction
edca_access_wait_fraction
backoff_countdown_fraction
backoff_frozen_phy_fraction
backoff_frozen_nav_fraction
jain_fairness
bss_mean_edca_access_wait_fraction
bss_total_data_tx_airtime_fraction
```

## Matrix and baseline

The matrix order is:

```text
sta_count_per_bss: 1, 5, 10, 15, 20, 25, 30
rssi_range: high, medium, low
interference_mode: isolated, ap_only_cochannel
traffic_mode: ul, dl, ul_dl
mimo_mode: su
repetition_attempt: 1 through repetitions
```

There are 126 configurations per repetition. Each direct JSON is independent
and cannot contain a cross-run baseline result.

The Python runner computes, per BSS row:

```text
bss_competition_overhead_vs_single_sta =
  1 - aggregate_interval_rate_N / matching_aggregate_interval_rate_1
```

The matching baseline has the same RSSI, interference mode, traffic mode,
MIMO mode, repetition attempt, and BSS ID. A positive one-STA baseline gives
`0.0` for its own row. A zero baseline makes the value empty/null because the
ratio is undefined. Results are signed and are not clamped; a negative result
is an aggregate gross attempted-data gain.

Because retransmission bytes are included, this field is not TCP goodput loss
or pure contention. In DL it describes gross STA TCP-ACK data attempts.

## CSV contract

The CSV retains one row per experiment attempt and BSS. The identity block is
unchanged (nine columns). It then has four BSS columns:

```text
bss_mean_dominant_data_phy_rate_mbps
bss_mean_effective_phy_rate_mbps
bss_aggregate_data_tx_rate_over_interval_mbps
bss_competition_overhead_vs_single_sta
```

Each of 30 fixed station blocks has six columns:

```text
sta_i_dominant_data_phy_rate_mbps
sta_i_dominant_data_profile_share
sta_i_effective_phy_rate_mbps
sta_i_data_tx_rate_over_interval_mbps
sta_i_data_tx_opportunity_gap_fraction
sta_i_tx_profile
```

The exact width is `9 + 4 + 30 * 6 = 193` columns. The file uses a semicolon
delimiter, UTF-8 BOM, CRLF, decimal dots, and deterministic matrix order.
Nonexistent station blocks are empty. An existing inactive station has numeric
`0.0` interval rate and otherwise empty fields.

`sta_i_tx_profile` uses ascending width/NSS/MCS order:

```text
W20_NSS1_MCS9:bytes=100,ppdus=2,airtime_us=40|W80_NSS2_MCS11:bytes=100500,ppdus=120,airtime_us=900
```

The Python validator builds this string from structured JSON; C++ does not
emit CSV syntax.

## Resource-aware parallel runner

The runner builds `saturated-tcp-scenario` once, then launches simulations as:

```text
./ns3 run --no-build "saturated-tcp-scenario ..."
```

The controller is the sole owner of run-directory allocation, the baseline
lookup, ordered publication buffer, and `results.csv`. Worker threads own one
subprocess group, its stdout/stderr files, process-tree resource monitoring,
and JSON validation. Workers never write CSV.

### RAM measurement

Every 100 ms, Linux `/proc` supplies process descendants, per-PID `VmRSS`,
`MemTotal`, and `MemAvailable`. Per-attempt `peak_rss_bytes` is the maximum
conservative sum across the worker's process tree. Each attempt retains
`resource_usage.json`; the run root retains `resource_summary.json`.
Operational resource fields never enter `results.csv`.

On a full run, the selected 30-STA/low/AP-only/UL+DL attempt for repetition one
runs first as the memory calibration job and is reused as its real matrix
result. Its peak RSS times `1.25` is the initial per-worker estimate.

Automatic admission uses:

```text
target reserve: 20 percent of MemTotal
hard accepted floor: approximately 15 percent of MemTotal
CPU reserve: two logical cores
```

Before a launch, current `MemAvailable` minus estimated unobserved growth of
active jobs and the next worker estimate must remain at or above the target
reserve. A reserve breach pauses new launches. Observed peaks raise later
estimates. `/proc` absence falls back to one sequential worker with a clear
diagnostic.

CLI controls are:

```text
--jobs N                    0/omitted means automatic; positive N is a cap
--memory-reserve-percent P default 20; accepted range 15 through 50
--experiment-ids LIST      explicit development subset
```

A subset run records `complete_matrix=false` and its selected IDs. No filter
means the complete matrix.

Subset selection automatically includes every matching one-STA baseline
needed by a requested non-baseline ID. The run manifest separately records
`requested_experiment_ids`, `executed_experiment_ids`, and
`auto_included_baseline_ids`, so dependencies cannot be mistaken for user
selection.

`resource_usage.json` has ordered fields:

```text
schema_version
experiment_id
repetition_attempt
sample_interval_ms
peak_rss_bytes
minimum_mem_available_bytes
minimum_mem_available_percent
wall_time_seconds
exit_code
monitor_mode
```

`resource_summary.json` has ordered fields:

```text
schema_version
complete_matrix
requested_experiment_ids
executed_experiment_ids
auto_included_baseline_ids
memory_reserve_percent
calibrated_peak_rss_bytes
worker_peak_estimate_bytes
maximum_parallel_workers
minimum_mem_available_bytes
minimum_mem_available_percent
attempts
```

Both resource schema versions are integer `1`. Attempt records are ordered by
experiment ID and repetition attempt. `monitor_mode` is `linux_proc` or
`sequential_fallback`.

Admission uses the exact inequality:

```text
MemAvailable
- sum(max(0, worker_peak_estimate - current_active_worker_rss))
- worker_peak_estimate
>= ceil(MemTotal * memory_reserve_percent / 100)
```

The 15 percent value is an acceptance floor, not a second admission formula.
If runtime use drops below it, the controller pauses new admissions, lets
active attempts finish, records the breach, and makes a complete run fail
acceptance. It does not kill otherwise healthy attempts solely to reclaim RAM.

### Parallel ordering and failure

After calibration, matching one-STA baselines run as the first wave. Remaining
attempts may complete out of order. The controller buffers validated three-row
attempt batches and appends only the next contiguous `(experiment_id,
repetition_attempt)` batch, preserving deterministic CSV order and atomic
three-BSS publication.

On first failure or interruption, the runner stops admissions, terminates all
active process groups with the established TERM/KILL lifecycle, retains every
created log/JSON/resource file, publishes only the contiguous valid prefix,
and exits nonzero.

## Development and acceptance runs

Implementation uses synthetic unit tests first. Real runs before final
acceptance are limited to:

- functional one-STA and five-STA high/isolated/UL smoke runs;
- sequential memory calibration at 1, 5, 15, and 30 STA/BSS using
  low/AP-only/UL+DL;
- a six-to-twelve-attempt parallel subset.

Temporary smoke outputs are removed after their evidence is recorded. Full
matrix runs occur only after deterministic tests, style, build, both examples,
and task reviews are clean.

The final accepted one-repetition run must retain:

```text
126 output JSON files
252 stdout/stderr logs
126 resource_usage.json files
378 data rows plus one CSV header
193 columns per CSV row
resource_summary.json
```

An independent read-only audit must reproduce every profile, station formula,
BSS mean/sum, matching baseline result, CSV cell, row order, and resource
count with zero structural or arithmetic discrepancies. It reports the minimum
available-memory percentage honestly; a result materially below the 15 percent
floor is not accepted without correcting scheduler admission and rerunning.

The accepted CSV replaces
`traces/saturated_tcp_benchmark_results.csv`. Prior run directories and their
artifacts are preserved.
