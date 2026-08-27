# Saturated Data PHY Benchmark v2 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace blended station PHY metrics with data-only width/NSS/MCS profiles and derived rates, add one-STA baselines, and produce a resource-aware parallel 126-experiment benchmark with a deterministic 193-column CSV.

**Architecture:** C++ observes station `PhyTxPsduBegin` events and reduces qualifying HE data PPDU attempts into per-window width/NSS/MCS profiles. Shared JSON schema version 2 carries structured station profiles and station-derived BSS values. A Python controller validates every JSON, computes matching-baseline results, measures process-tree RSS, schedules independent simulations under a RAM reserve, and remains the sole ordered CSV writer.

**Tech Stack:** ns-3 C++/CMake, nlohmann JSON through the existing streaming writer, Python 3 standard library, Linux `/proc`, semicolon CSV with UTF-8 BOM and CRLF.

**Spec:** `docs/superpowers/specs/2026-08-27-saturated-data-phy-benchmark-v2-design.md`

## Global Constraints

- Work in the paired isolated workspace rooted at `/home/bsa/projects/ns-3-dev-worktrees/saturated-data-phy-v2`; run nested Git commands from its `contrib/llm`, and ns-3 build/test/run commands from the outer root.
- Preserve every pre-existing run directory and the published v1 results until a v2 full run passes audit; never overwrite an attempt directory or result file.
- Use TDD for every behavior change: record the focused RED command and expected failure before production edits, then GREEN output in the task report.
- Keep the existing three-BSS topology, native RSSI placement, AP-only cochannel filter, exact one-second epoch, 10 ms default windows, SU-only mode, MinstrelHt, TcpHighSpeed, and station-only observation scope.
- Fix the operating channel at 80 MHz and HE data at 3200 ns GI; enable RTS/CTS with a zero-byte threshold; profile Minstrel-selected 20/40/80 MHz data widths and reject non-HE/invalid-width/wrong-GI data TxVectors.
- Count only STA-transmitted unicast data MPDUs, including TCP ACK data and every data retransmission; exclude all control and management frames.
- JSON root order is unchanged but `schema_version` is exactly `2`; no backward compatibility aliases for the four removed benchmark PHY fields.
- CSV is exactly 193 columns, semicolon-delimited, UTF-8 BOM, CRLF, decimal dot, deterministic experiment/attempt/BSS order, and one atomic three-BSS publication per attempt.
- Matrix STA counts are exactly `1, 5, 10, 15, 20, 25, 30`, giving 126 configurations per repetition; repetitions remain adjustable and default to one.
- The controller alone writes CSV and resource summary files. Worker threads may write only their assigned attempt directory and return validated values.
- Automatic scheduling targets 20 percent `MemTotal` available and accepts no final full run that materially falls below the 15 percent floor. Positive `--jobs` is only a cap and never disables memory admission.
- Real experiments during implementation are limited to focused smoke/calibration/subset runs. Run the complete 126 matrix only after all deterministic gates and reviews pass.
- All C++ public classes, methods, parameters, returns, and members follow ns-3 Doxygen rules; comments and Doxygen use ASCII mathematical notation.
- Each task ends with an ns-3-style imperative commit whose subject begins `llm:` and is at most 72 characters.

---

### Task 1: Fix HE guard interval and RTS/CTS configuration

**Files:**
- Modify: `config/saturated_tcp_config.toml`
- Modify: `examples/saturated-tcp/config.h`
- Modify: `examples/saturated-tcp/config.cc`
- Modify: `examples/saturated-tcp/config-toml.cc`
- Modify: `examples/saturated-tcp/config-cli.cc`
- Modify: `examples/saturated-tcp/config-json.cc`
- Modify: `examples/saturated-tcp/config-validation.cc`
- Modify: `examples/saturated-tcp/topology.cc`
- Modify: `test/saturated-tcp/config-test-suite.cc`
- Modify: `test/saturated-tcp/topology-test-suite.cc`

**Interfaces:**
- Produces: `SaturatedWifiConfig::guardIntervalNs` (`uint32_t`, exactly 3200) and `SaturatedWifiConfig::rtsCtsThresholdBytes` (`uint32_t`, exactly 0).
- Produces: exact TOML/CLI names `wifi.guard_interval_ns`, `wifi.rts_cts_threshold_bytes`, `--wifi-guard-interval-ns`, and `--wifi-rts-cts-threshold-bytes`.
- Produces: AP and every STA `HeConfiguration::GuardInterval == NanoSeconds(3200)` and remote-station-manager `RtsCtsThreshold == 0` after topology installation.

- [ ] **Step 1: Write failing configuration tests**

Add literal default/TOML/CLI/JSON expectations for 3200 and 0. Add rejection cases for 800, 1600, 3201, and a nonzero RTS threshold. The production change each test catches is omission or drift of an explicit invariant.

- [ ] **Step 2: Write a failing topology behavior test**

Build the smallest topology and assert every AP/STA device reports 3200 ns GI and zero RTS threshold from live ns-3 objects, not source text.

- [ ] **Step 3: Run RED**

Run from the outer root:

```bash
./ns3 build llm-test
./test.py -s llm --no-build
```

Expected: build/test failure because the two config members and topology effects do not exist.

- [ ] **Step 4: Implement exact parsing, validation, metadata, and topology effects**

Add the two fields through the existing compiled-default -> TOML -> CLI path. Use:

```cpp
wifi.SetRemoteStationManager(config.wifi.rateManager,
                             "RtsCtsThreshold",
                             UintegerValue(config.wifi.rtsCtsThresholdBytes));
```

After installation, set the AP and every STA HE configuration through
`SetGuardInterval(NanoSeconds(config.wifi.guardIntervalNs))`. Validate the live values in the existing placement/invariant phase.

- [ ] **Step 5: Run GREEN and style**

```bash
./ns3 build llm-test saturated-tcp-scenario
./test.py -s llm --no-build
./utils/check-style-clang-format.py contrib/llm/examples/saturated-tcp \
  contrib/llm/test/saturated-tcp
```

- [ ] **Step 6: Commit**

```bash
git add config/saturated_tcp_config.toml examples/saturated-tcp \
  test/saturated-tcp/config-test-suite.cc test/saturated-tcp/topology-test-suite.cc
git commit -m "llm: Fix saturated Wi-Fi invariants"
```

---

### Task 2: Collect data-only width/NSS/MCS profiles

**Files:**
- Create: `examples/saturated-tcp/data-tx-metrics.h`
- Create: `examples/saturated-tcp/data-tx-metrics-internal.h`
- Create: `examples/saturated-tcp/data-tx-metrics.cc`
- Create: `test/saturated-tcp/data-tx-metrics-test-suite.cc`
- Modify: `test/llm-test-suite.cc`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces:

```cpp
struct DataTxProfileKey
{
    uint16_t channelWidthMhz;
    uint8_t nss;
    uint8_t mcs;
    auto operator<=>(const DataTxProfileKey&) const = default;
};

struct DataTxProfileAccumulator
{
    long double transmittedPsduBytes{0.0L};
    uint64_t ppduAttemptCount{0};
    int64_t ppduAirtimeNs{0};
    long double nominalRateBps{0.0L};
    void Merge(const DataTxProfileAccumulator& other);
};

using DataTxProfileMap = std::map<DataTxProfileKey, DataTxProfileAccumulator>;
```

- Produces: `StationDataTxMetricRecorder` with station registration, `RecordPpduAttempt`, window access, and independent overall accumulation.
- Consumes: exact 80 MHz operating-width and 3200 ns guard-interval invariants from Task 1.

- [ ] **Step 1: Write failing extraction tests**

Use real `WifiPsdu`, `WifiMpdu`, and `WifiTxVector` fixtures with hand-derived bytes/rates. Cover:

- one HE data MPDU;
- TCP-ACK-like data MPDU;
- MAC ACK, BlockAck, RTS, CTS, management, group, and AP-address data exclusion;
- A-MPDU subframe bytes;
- repeated identical data attempt counted twice;
- 20/40/80 MHz, NSS1/MCS9, and NSS2/MCS11 separation;
- non-HE, width outside the 80 MHz operating channel, wrong GI, multiple non-null SU PSDUs, and invalid duration rejection.

Each test names the wrong production mutation it catches and uses literal expectations.

- [ ] **Step 2: Write failing window/overall tests**

Use an event crossing a 10 ms boundary. Assert proportional bytes/airtime splitting, start-window attempt count, one overall attempt, exact profile merging, and clipped one-second endpoint behavior.

- [ ] **Step 3: Run RED**

```bash
./ns3 build llm-test
```

Expected: compile failure because the data-profile interfaces are absent.

- [ ] **Step 4: Implement the minimal profile collector**

Filter on `header.HasData()`, unicast `Addr1`, and the registered transmitter in `Addr2`. Obtain:

```cpp
const auto mode = txVector.GetMode(staId);
const auto nss = txVector.GetNss(staId);
const auto mcs = mode.GetMcsValue();
const auto rate = mode.GetDataRate(txVector, staId);
```

Require HE SU modulation, a 20/40/80 MHz actual width, and 3200 ns. Calculate complete PPDU airtime once. Keep profile accumulation and interval splitting independent of JSON/output types.

- [ ] **Step 5: Run GREEN and style**

```bash
./ns3 build llm-test
./test.py -s llm --no-build
./utils/check-style-clang-format.py contrib/llm/examples/saturated-tcp \
  contrib/llm/test/saturated-tcp
```

- [ ] **Step 6: Commit**

```bash
git add CMakeLists.txt examples/saturated-tcp/data-tx-metrics* \
  test/llm-test-suite.cc test/saturated-tcp/data-tx-metrics-test-suite.cc
git commit -m "llm: Collect station data TX profiles"
```

---

### Task 3: Replace shared PHY JSON with schema version 2

**Files:**
- Modify: `examples/statistics/output-types.h`
- Modify: `examples/statistics/json/document.cc`
- Modify: `examples/statistics/json/phy.cc`
- Modify: `scripts/live_verification/schema.py`
- Modify: `scripts/live_verification/common.py` if its exact PHY key tuple is centralized there
- Modify: `test/statistics/experiment-hierarchy-json-test-suite.cc`
- Modify: `test/statistics/experiment-json-test-suite.cc`
- Modify: `scripts/tests/live_verification/test_schema_root.py`
- Modify: all literal shared-schema JSON fixtures under `scripts/tests/`

**Interfaces:**
- Produces: `DataTxProfileOutput` and the nine fixed v2 `PhyCategoryOutput` fields from the spec.
- Produces: exact ordered profile-entry JSON and root `schema_version: 2`.
- Removes: the four v1 benchmark PHY members and serialized keys without aliases.

- [ ] **Step 1: Write failing C++ serialization tests**

Construct one station-shaped PHY category and one BSS-shaped category. Assert exact key order, numeric/null roles, structured profile order, and root version 2. Assert ordinary default entities emit eight null numerics and an empty profile.

- [ ] **Step 2: Write failing live-schema tests**

Add valid station/BSS/default fixtures and reject:

- version 1;
- missing/extra/reordered v2 keys;
- malformed profile entries;
- duplicate/unsorted width/NSS/MCS keys;
- non-finite/negative bytes, airtime, rates, shares, or gaps;
- zero NSS, MCS above 11, and impossible populated station/BSS role combinations.

- [ ] **Step 3: Run RED**

```bash
./ns3 build llm-test llm-scenario
PYTHONDONTWRITEBYTECODE=1 python3 contrib/llm/scripts/live_test_traces.py --self-test
```

Expected: compilation/schema failures on absent v2 fields and version.

- [ ] **Step 4: Implement DTO and streaming JSON replacement**

Use a vector of ordered `DataTxProfileOutput` entries. Serialize all nine keys before existing `busy_time_us`; do not change directional PHY categories. Change the root literal to integer 2 and make Python validation exact.

- [ ] **Step 5: Run GREEN and existing scenario smoke**

```bash
./ns3 build llm-test llm-scenario
./test.py -s llm --no-build
./test.py -e 'llm-scenario*' --no-build
PYTHONDONTWRITEBYTECODE=1 python3 contrib/llm/scripts/live_test_traces.py --self-test
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover \
  -s contrib/llm/scripts/tests -t contrib/llm/scripts -p 'test_*.py'
```

- [ ] **Step 6: Commit**

```bash
git add examples/statistics scripts/live_verification scripts/tests \
  test/statistics
git commit -m "llm: Replace benchmark PHY JSON fields"
```

---

### Task 4: Integrate data metrics and remove access-wait tracking

**Files:**
- Modify: `examples/saturated-tcp/benchmark-statistics.h`
- Modify: `examples/saturated-tcp/benchmark-statistics.cc`
- Modify: `examples/saturated-tcp/output.cc`
- Modify: `examples/saturated-tcp/topology.cc`
- Modify: `examples/saturated-tcp-scenario.cc` if setup types change
- Modify: `test/saturated-tcp/benchmark-output-test-suite.cc`
- Modify: `test/saturated-tcp/topology-test-suite.cc`
- Modify: `test/saturated-tcp/smoke_json.py`
- Modify: `test/saturated-tcp/smoke_json_test.py`
- Modify: `CMakeLists.txt`
- Modify: `examples/CMakeLists.txt`
- Delete: `examples/saturated-tcp/sta-phy-metrics.h`
- Delete: `examples/saturated-tcp/sta-phy-metrics-internal.h`
- Delete: `examples/saturated-tcp/sta-phy-metrics.cc`
- Delete: `examples/saturated-tcp/access-tracking-sta-wifi-mac.h`
- Delete: `examples/saturated-tcp/access-tracking-sta-wifi-mac.cc`
- Delete: `examples/saturated-tcp/access-wait-tracker.h`
- Delete: `examples/saturated-tcp/access-wait-tracker.cc`
- Delete: `test/saturated-tcp/sta-phy-metrics-test-suite.cc`
- Delete: `test/saturated-tcp/access-wait-test-suite.cc`

**Interfaces:**
- Consumes: Task 2 profile accumulators and Task 3 output DTO.
- Produces: `DeriveStationDataTxMetrics` fields and station-derived BSS mean/sum fields for every sparse window and dense overall.
- Produces: ordinary `ns3::StaWifiMac` topology with only station PHY trace subscriptions.

- [ ] **Step 1: Write failing derived-metric tests**

Use literal profile maps to assert:

- dominant bytes/rate/share and every tie-break;
- `effective = 8B/Tdata`;
- `interval = 8B/Tinterval`;
- `gap = 1 - interval/effective`;
- zero-data null/zero/empty behavior;
- profile-sum reconstruction and rejection of material formula violations.

- [ ] **Step 2: Write failing BSS/output tests**

Assert defined-only means, all-station interval sum, all-undefined null means/numeric-zero aggregate, AP-originated observations ignored, sparse windows, dense overall, and exact raw overall/window reconstruction.

Sparse-window expectations are data-only: omit a station without a data
profile, omit an AP with no active station, and omit a completely inactive
window. Keep every configured entity in dense `overall`.

- [ ] **Step 3: Run RED**

```bash
./ns3 build llm-test saturated-tcp-scenario
./test.py -s llm --no-build
```

Expected: failures because benchmark statistics still consume v1 accumulators and fields.

- [ ] **Step 4: Implement profile derivation and benchmark validation**

Sort output profiles by `(channel_width_mhz, nss, mcs)`. Compute dominant selection with the spec tie-break. Use scaled floating tolerance only for proportional long-double sums; retain exact integer attempt comparisons. Populate station and BSS roles without duplicating runner formulas.

- [ ] **Step 5: Remove obsolete access tracking**

Connect only `PhyTxPsduBegin`; remove MAC/TXOP ownership and callbacks. Install `ns3::StaWifiMac`. Remove deleted sources/tests from CMake and the suite registration.

- [ ] **Step 6: Run GREEN, smoke, and style**

```bash
./ns3 build llm-test saturated-tcp-scenario
./test.py -s llm --no-build
./test.py -e 'saturated-tcp-scenario*' --no-build
./utils/check-style-clang-format.py contrib/llm/examples/saturated-tcp \
  contrib/llm/test/saturated-tcp
```

- [ ] **Step 7: Commit**

```bash
git add -A CMakeLists.txt examples/saturated-tcp test/saturated-tcp \
  test/llm-test-suite.cc
git commit -m "llm: Derive data PHY benchmark metrics"
```

---

### Task 5: Update matrix, strict validator, baseline, and 193-column CSV

**Files:**
- Modify: `exp_scripts/saturated_tcp_benchmark/matrix.py`
- Modify: `exp_scripts/saturated_tcp_benchmark/validation.py`
- Modify: `exp_scripts/saturated_tcp_benchmark/csv_output.py`
- Modify: `exp_scripts/tests/test_saturated_tcp_matrix.py`
- Modify: `exp_scripts/tests/test_saturated_tcp_validation.py`
- Modify: `exp_scripts/tests/test_saturated_tcp_csv.py`

**Interfaces:**
- Produces: exact 126-configuration `build_matrix()` and unchanged attempt/RNG mapping.
- Produces: `StationCsvMetrics` with six station fields and `BssCsvRow` with four BSS fields.
- Produces:

```python
BaselineKey = tuple[str, str, str, str, int, int]

def apply_matching_baseline(
    rows: tuple[BssCsvRow, ...],
    baselines: Mapping[BaselineKey, float],
) -> tuple[BssCsvRow, ...]: ...
```

- Produces: deterministic compact `Wx_NSSy_MCSz` profile rendering from structured JSON.
- Produces: subset dependency expansion into requested, executed, and
  auto-included matching-baseline ID tuples.

- [ ] **Step 1: Write failing exact-matrix and baseline tests**

Assert literal STA-count order, 126 stable IDs, 18 baselines per repetition, same-attempt/BSS lookup, baseline `0.0`, signed positive/negative results, zero-baseline empty result, and missing/mismatched baseline failure.

- [ ] **Step 2: Write failing strict JSON validation tests**

Update complete fixtures to v2. Independently recompute every station profile/field and BSS mean/sum. Reject profile/derived mismatches, old fields, wrong role population, malformed null/zero shapes, and DL metadata that changes station-only semantics.

- [ ] **Step 3: Write failing 193-column CSV tests**

Freeze the literal header, exact row width, station block boundaries, profile string, empty nonexistent station cells, inactive existing station `0.0`, BOM, semicolon, CRLF, decimal dot, and atomic three-row append.

- [ ] **Step 4: Run RED**

```bash
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover \
  -s contrib/llm/exp_scripts/tests -t contrib/llm/exp_scripts -p 'test_*.py' -v
```

Expected: matrix count, schema, dataclasses, and header failures.

- [ ] **Step 5: Implement minimal matrix/validation/CSV replacement**

Do not compute baseline values in the C++ JSON loader. Return strict direct-run rows first, then apply the matching baseline in the controller. Keep CSV file identity/no-follow/fsync protections intact.

- [ ] **Step 6: Run GREEN**

```bash
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover \
  -s contrib/llm/exp_scripts/tests -t contrib/llm/exp_scripts -p 'test_*.py' -v
```

- [ ] **Step 7: Commit**

```bash
git add exp_scripts/saturated_tcp_benchmark exp_scripts/tests
git commit -m "llm: Add single-STA benchmark baselines"
```

---

### Task 6: Measure process-tree RAM and retain resource records

**Files:**
- Create: `exp_scripts/saturated_tcp_benchmark/resources.py`
- Create: `exp_scripts/tests/test_saturated_tcp_resources.py`
- Modify: `exp_scripts/saturated_tcp_benchmark/runner.py`
- Modify: `exp_scripts/tests/test_saturated_tcp_runner.py`

**Interfaces:**
- Produces:

```python
@dataclass(frozen=True)
class MemorySnapshot:
    mem_total_bytes: int
    mem_available_bytes: int

@dataclass(frozen=True)
class AttemptResourceUsage:
    sample_interval_ms: int
    peak_rss_bytes: int
    wall_time_seconds: float
    exit_code: int

def read_memory_snapshot(meminfo_path: Path = Path("/proc/meminfo")) -> MemorySnapshot: ...
def process_tree_rss_bytes(root_pid: int, proc_root: Path = Path("/proc")) -> int: ...
```

- Produces: exclusive atomic `resource_usage.json` per attempt and controller-owned `resource_summary.json`.

The attempt file uses the exact ordered keys and values from the spec,
including integer schema version 1 and monitor mode `linux_proc` or
`sequential_fallback`. The root summary uses the exact ordered keys from the
spec and attempt records in experiment/attempt order.

- [ ] **Step 1: Write failing literal `/proc` parser tests**

Build temporary fake proc trees. Assert kB-to-byte conversion, recursive descendants, duplicate-PID prevention, vanished-process tolerance, malformed/missing field diagnostics, and conservative RSS sum.

- [ ] **Step 2: Write failing live-process monitor test**

Start a small real parent/child process tree that allocates known memory. Assert positive peak, wall time, exit code, child inclusion, and no surviving descendant after cleanup. Do not assert an exact allocator-dependent RSS.

- [ ] **Step 3: Write failing resource-file lifecycle tests**

Assert exclusive/no-overwrite/no-symlink publication, deterministic JSON keys, records on nonzero exit/timeout/interruption, and retained partial root summary.

- [ ] **Step 4: Run RED**

```bash
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest \
  exp_scripts.tests.test_saturated_tcp_resources -v
```

Expected: import failure because `resources.py` is absent.

- [ ] **Step 5: Implement Linux resource monitoring**

Sample every 100 ms while preserving existing process-group timeout and signal semantics. Treat `/proc` disappearance after process exit as normal. If `/proc` capability is unavailable at runner startup, return an explicit sequential-only capability rather than fabricated zero memory.

- [ ] **Step 6: Run GREEN and full runner tests**

```bash
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest \
  exp_scripts.tests.test_saturated_tcp_resources -v
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover \
  -s contrib/llm/exp_scripts/tests -t contrib/llm/exp_scripts -p 'test_*.py' -v
```

- [ ] **Step 7: Commit**

```bash
git add exp_scripts/saturated_tcp_benchmark/resources.py \
  exp_scripts/saturated_tcp_benchmark/runner.py exp_scripts/tests
git commit -m "llm: Measure benchmark process memory"
```

---

### Task 7: Parallelize experiments with one ordered CSV owner

**Files:**
- Create: `exp_scripts/saturated_tcp_benchmark/scheduler.py`
- Create: `exp_scripts/tests/test_saturated_tcp_scheduler.py`
- Modify: `exp_scripts/saturated_tcp_benchmark/runner.py`
- Modify: `exp_scripts/tests/test_saturated_tcp_runner.py`
- Modify: `exp_scripts/saturated_tcp_experiment.py` only if entry-point argument forwarding changes

**Interfaces:**
- Consumes: Task 5 baseline/rows and Task 6 memory snapshots/usage.
- Produces: `--jobs`, `--memory-reserve-percent`, and `--experiment-ids` CLI behavior.
- Produces: build-once then `./ns3 run --no-build` worker commands.
- Produces: calibration reuse, baseline wave, bounded worker pool, memory admission, active process-group registry, ordered completion buffer, and single controller CSV publication.

- [ ] **Step 1: Write failing pure scheduler tests**

Use literal snapshots/usages to assert:

- default 20 percent target and 15 percent CLI minimum;
- two-core CPU reserve;
- user jobs as cap only;
- predicted active growth reservation;
- launch pause and resume;
- observed peak estimate increase;
- 1.25 calibration margin;
- `/proc` fallback to one worker.

- [ ] **Step 2: Write failing execution-order tests**

With controlled worker completions, assert calibration runs once and is reused, baseline wave precedes dependent publication, out-of-order completion yields exact ordered CSV, one controller thread calls `append_attempt`, and three BSS rows remain atomic.

- [ ] **Step 3: Write failing failure/interruption tests**

Assert first failure stops admissions, terminates every active real process group, retains all logs/JSON/resource files, publishes only the contiguous prefix, and returns nonzero. Repeat for SIGINT and timeout.

- [ ] **Step 4: Write failing subset/command tests**

Assert `--experiment-ids` parsing, duplicate/unknown rejection, manifest `complete_matrix=false`, selected IDs, default full selection, and exact `--no-build` command. Assert build happens once before workers.

For each requested non-baseline ID, assert automatic inclusion of the matching
one-STA ID and exact requested/executed/auto-included manifest tuples.

- [ ] **Step 5: Run RED**

```bash
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest \
  exp_scripts.tests.test_saturated_tcp_scheduler \
  exp_scripts.tests.test_saturated_tcp_runner -v
```

Expected: missing scheduler and parallel-runner behavior failures.

- [ ] **Step 6: Implement bounded worker supervision**

Use a controller-owned bounded thread executor or equivalent simple supervisor. Workers return immutable validated results; they do not mutate baselines, buffers, summaries, or CSV. Re-check `MemAvailable` before every admission and account for expected unobserved growth of active jobs.

Implement the exact admission inequality from the spec. A runtime drop below
15 percent pauses admissions and marks a complete run unacceptable but does
not kill healthy active attempts solely for memory reclamation.

- [ ] **Step 7: Run GREEN and mutation checks**

```bash
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest \
  exp_scripts.tests.test_saturated_tcp_scheduler \
  exp_scripts.tests.test_saturated_tcp_runner -v
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover \
  -s contrib/llm/exp_scripts/tests -t contrib/llm/exp_scripts -p 'test_*.py' -v
```

Mentally mutate reserve math, completion key, CSV owner, stop flag, and process registry; confirm a named test catches each mutation.

- [ ] **Step 8: Commit**

```bash
git add exp_scripts/saturated_tcp_benchmark exp_scripts/tests \
  exp_scripts/saturated_tcp_experiment.py
git commit -m "llm: Run saturated experiments in parallel"
```

---

### Task 8: Update documentation, audit tooling, and limited live evidence

**Files:**
- Modify: `README.md`
- Modify: `README_RU.md`
- Modify: `docs/superpowers/specs/2026-08-26-saturated-tcp-benchmark-design.md` with a superseded-by pointer, not rewritten history
- Modify: `docs/superpowers/plans/2026-08-26-saturated-tcp-benchmark.md` with a superseded-by pointer, not rewritten history
- Create: `exp_scripts/audit_saturated_tcp_results.py`
- Create: `exp_scripts/saturated_tcp_benchmark/audit.py`
- Create: `exp_scripts/tests/test_saturated_tcp_audit.py`
- Modify: `test/saturated-tcp/smoke_json.py`
- Modify: `test/saturated-tcp/smoke_json_test.py`

**Interfaces:**
- Produces: bilingual exact field/formula/matrix/parallel/resource documentation.
- Produces: read-only audit command accepting a retained run directory and independently reconstructing JSON/CSV/resource invariants.

- [ ] **Step 1: Write failing audit tests**

Create a literal two-configuration mini-run fixture containing one positive baseline and one dependent result. Assert zero-discrepancy output, exact profile/formula/CSV/resource reconstruction, and failures for one-cell, baseline, row-order, resource-count, and profile-sum mutations.

- [ ] **Step 2: Run RED**

```bash
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest \
  exp_scripts.tests.test_saturated_tcp_audit -v
```

Expected: import/entry-point failure because audit tooling is absent.

- [ ] **Step 3: Implement the minimal independent auditor**

Reuse only schema parsing primitives; independently calculate formulas and exact CSV cell strings so the audit does not call the same row builder it verifies. Report counts, discrepancies, nulls, signed baseline ranges, minimum available-memory percent, and resource peaks.

- [ ] **Step 4: Update EN/RU documentation and smoke validator**

Explain dominant profile, gross effective rate, interval rate, opportunity gap, gross retransmission caveat, DL TCP-ACK scope, matching baseline, 126/378/193 counts, memory controls, subset marker, resource files, and full commands. Keep EN/RU commands and field names identical.

- [ ] **Step 5: Run all deterministic gates**

```bash
PYTHONDONTWRITEBYTECODE=1 python3 contrib/llm/scripts/live_test_traces.py --self-test
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover \
  -s contrib/llm/scripts/tests -t contrib/llm/scripts -p 'test_*.py' -v
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover \
  -s contrib/llm/exp_scripts/tests -t contrib/llm/exp_scripts -p 'test_*.py' -v
./utils/check-style-clang-format.py contrib/llm/examples contrib/llm/model contrib/llm/test
./ns3 build test-runner llm-test llm-scenario saturated-tcp-scenario
./test.py -s llm --no-build
./test.py -e 'llm-scenario*' --no-build
./test.py -e 'saturated-tcp-scenario*' --no-build
git -C contrib/llm diff --check
```

- [ ] **Step 6: Run limited functional experiments**

Run one-STA and five-STA high/isolated/UL attempts. Audit them, record evidence in the task report, and remove only their explicitly temporary output directories.

- [ ] **Step 7: Run sequential memory calibration**

Run the explicit subset at 1, 5, 15, and 30 STA/BSS with low/AP-only/UL+DL and `--jobs 1`. Record peak RSS/wall time/output size, verify resource JSON, and retain the summary in the task report. Remove only temporary calibration run trees after evidence is copied to the SDD report.

- [ ] **Step 8: Run a small parallel subset**

Run six to twelve IDs spanning baseline and dependent configurations with automatic jobs. Require ordered CSV, no duplicate calibration, all resource files, clean process exit, and recorded minimum available RAM at or above 15 percent. Remove only the explicitly temporary subset tree after evidence is recorded.

- [ ] **Step 9: Commit**

```bash
git add README.md README_RU.md docs/superpowers exp_scripts test/saturated-tcp
git commit -m "llm: Document data PHY benchmark v2"
```

---

### Task 9: Run, audit, and publish the full 126-experiment result

**Files:**
- Replace: `traces/saturated_tcp_benchmark_results.csv`
- Modify: `.gitattributes` only if the existing exact CSV rule does not cover the replacement path
- No production-code change is permitted after the accepted run without invalidating and rerunning the matrix.

**Interfaces:**
- Consumes: complete reviewed implementation and audit tool from Tasks 1-8.
- Produces: retained accepted run, exact audit report/resource evidence, and byte-identical published CSV.

- [ ] **Step 1: Re-run deterministic gates on the exact pre-run commit**

Run every Task 8 deterministic command. Require clean nested status and record outer pre-existing state hashes before the matrix.

- [ ] **Step 2: Launch the exact full run once**

From the isolated outer root, run the no-filter experiment command with default one repetition and automatic jobs. Retain its `run/scripted_exp_<timestamp>/` directory permanently. Do not delete failed full-run attempts; if correction is required, retain them and launch a new timestamped run only after a tested code fix.

- [ ] **Step 3: Audit immutable results**

Run the committed auditor read-only. Require:

```text
126 JSON
252 stdout/stderr logs
126 resource_usage.json
378 data rows plus header
193 columns
zero structure/formula/baseline/CSV discrepancies
minimum available RAM not materially below 15 percent
```

Classify empirical reversals/outliers without forcing monotonicity. Seal the run manifest and CSV SHA-256 in the task report.

- [ ] **Step 4: Copy the accepted CSV byte-for-byte**

Use a byte-preserving copy from accepted `results.csv` to
`traces/saturated_tcp_benchmark_results.csv`. Verify identical SHA-256, BOM,
CRLF, 379 lines, and 193 columns. Do not regenerate it through another code
path.

- [ ] **Step 5: Commit the artifact**

```bash
git add .gitattributes traces/saturated_tcp_benchmark_results.csv
git diff --cached --check
git commit -m "llm: Update saturated TCP benchmark results"
```

- [ ] **Step 6: Final verification after artifact commit**

Repeat all deterministic gates except the full matrix. Re-run the auditor on
the retained accepted run and re-check both CSV hashes.

- [ ] **Step 7: Report for whole-branch review**

Record accepted run path, counts, wall time, worker limit, calibrated/observed
RSS, minimum available-memory percentage, manifest hash, CSV hash, audit
result, empirical caveats, and confirmation that prior run data was untouched.
