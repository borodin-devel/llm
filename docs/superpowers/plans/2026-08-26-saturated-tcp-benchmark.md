# Saturated TCP Wi-Fi Benchmark Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build an independent 108-run saturated TCP 802.11ax benchmark that measures station-transmitted PPDU theoretical/practical rate, efficiency, and EDCA contention, writes the shared windowed JSON schema, and produces one Excel-compatible CSV.

**Architecture:** A new `saturated-tcp-scenario` example composes focused configuration, propagation, topology, sender, readiness, and station-metrics units without depending on trace replay or agent distribution. Shared output DTOs and the streaming JSON writer gain optional non-directional PHY fields. A standard-library Python runner executes the deterministic matrix sequentially, retains each JSON, writes fixed-width BSS rows, and validates formulas before publication.

**Tech Stack:** C++23, ns-3 Wi-Fi/Internet/Applications, CMake, nlohmann JSON, toml++, Python 3 `tomllib`/`csv`/`unittest`.

**Spec:** `docs/superpowers/specs/2026-08-26-saturated-tcp-benchmark-design.md`

## Global Constraints

- Work only on the nested `contrib/llm` feature branch; preserve outer `.gitignore`, `VAGUE_TASK.md`, traces, and pre-existing run artifacts.
- Do not modify ns-3 core Wi-Fi sources; station access instrumentation must use a benchmark STA MAC subclass plus existing TXOP traces.
- Do not use trace parsing, agent generators, traffic schedules, or agent distribution in the benchmark.
- Current ns-3 source explicitly lacks scheduled DL MU-MIMO; expose SU only, reject `mu`, and run exactly 108 default scenarios producing 324 CSV rows.
- Every AP and STA is 2x2; Wi-Fi is 802.11ax, 5 GHz channel 42, 80 MHz, 20 dBm, MinstrelHt, colors 1/2/3.
- Allowed propagation uses native deterministic Yans LogDistance; only cross-BSS AP-to-AP links exist in co-channel mode, and isolated BSSs use separate channels.
- Start payload and statistics at the first whole-second boundary after every TCP sender is connected; measure exactly one second.
- Measure only station-transmitted relevant PPDUs. No AP PPDU or TCP-goodput metric enters the new fields or CSV.
- Theoretical rate is airtime-weighted `WifiTxVector` nominal rate; practical rate is PSDU bits divided by complete PPDU airtime; efficiency is practical/theoretical.
- Contention is the union of pending STA EDCA access intervals divided by the window duration; it includes AIFS/backoff/frozen backoff and cannot exceed one.
- Preserve the existing root JSON schema/version/order and validation keys. Add four optional non-directional `phy_stats` fields; ordinary `llm-scenario` emits null for unavailable fields.
- JSON stays bounded-memory streaming, two-space indented, exclusive-create, and no-clobber.
- CSV has exactly the approved fixed columns, semicolon delimiter, UTF-8 BOM, CRLF, decimal dots, and no diagnostic/TCP-goodput columns.
- Repetition attempts are separate rows; `rng_seed=12345`, `rng_run=repetition_attempt`; default repetitions is one.
- Use `git mv` for moves, `apply_patch` for edits, exact staging, complete ASCII Doxygen, and ns-3 style.
- Run `test.py` from the outer ns-3 root because it requires `.lock-ns3`.
- Do not run the full matrix before Task 11. Task 11 runs the honest 108-run matrix once for the final accepted code and retains its successful run directory.

---

### Task 1: Extend the shared JSON schema for benchmark PHY fields

**Files:**
- Modify: `examples/statistics/output-types.h`
- Modify: `examples/statistics/json/phy.cc`
- Modify: `examples/statistics/json/writer.h`
- Modify: `examples/statistics/json/writer.cc`
- Create: `examples/statistics/json/document.cc`
- Modify: `CMakeLists.txt`
- Modify: `examples/CMakeLists.txt`
- Modify: `test/statistics/experiment-hierarchy-json-test-suite.cc`
- Modify: `test/statistics/experiment-json-test-suite.cc`
- Modify: `scripts/live_verification/schema_categories.py`
- Modify: `scripts/tests/live_verification/fixtures.py`
- Modify: `scripts/tests/live_verification/test_schema_root.py`

**Interfaces:**
- Consumes: current `UnifiedExperimentSummary`, `JsonWriter`, and `ScenarioConfig` output path.
- Produces: four optional fields on `PhyCategoryOutput` and generic `ExperimentJsonSections` callbacks for measurement semantics/configuration.
- Preserves: `WriteExperimentHierarchyJson(std::ostream&, const UnifiedExperimentSummary&, const ScenarioConfig&)` for `llm-scenario`.

- [ ] **Step 1: Add failing C++ schema and null tests**

Add these members to the literal hierarchy fixture expectation before production code exists:

```cpp
constexpr std::array<std::string_view, 8> phyKeys{
    "average_theoretical_phy_rate_mbps",
    "average_practical_phy_rate_mbps",
    "channel_efficiency",
    "contention_fraction",
    "busy_time_us",
    "channel_utilization_percent",
    "uplink",
    "downlink",
};
```

In the literal summary, set one station and AP PHY category to:

```cpp
phy.averageTheoreticalPhyRateMbps = 960.8;
phy.averagePracticalPhyRateMbps = 720.6;
phy.channelEfficiency = 0.75;
phy.contentionFraction = 0.2;
```

Assert exact JSON values and assert default entities serialize all four as JSON null.

- [ ] **Step 2: Add failing Python schema expectations**

Extend the fixture's `phy_stats` with the same four keys and values. Add rejection cases for missing keys, object/string values, non-finite values, and fractions outside `[0,1]`; null remains accepted.

- [ ] **Step 3: Run RED tests**

From the outer root:

```bash
./ns3 build llm-test
./test.py -s llm --no-build
PYTHONDONTWRITEBYTECODE=1 python3 contrib/llm/scripts/live_test_traces.py --self-test
```

Expected: C++ compilation fails because the DTO members are absent; Python schema tests fail because the new keys are unknown/missing.

- [ ] **Step 4: Add DTO fields and serialize them first in `phy_stats`**

Add:

```cpp
std::optional<double> averageTheoreticalPhyRateMbps;
std::optional<double> averagePracticalPhyRateMbps;
std::optional<double> channelEfficiency;
std::optional<double> contentionFraction;
```

Give every member complete Doxygen. In `WritePhyCategoryJson`, emit the four exact snake-case keys before `busy_time_us`, using `Value()` or `Null()` explicitly.

- [ ] **Step 5: Split and generalize the root document writer without changing output**

Add:

```cpp
struct ExperimentJsonSections
{
    std::function<void(JsonWriter&)> writeMeasurementSemantics;
    std::function<void(JsonWriter&)> writeConfiguration;
};

void WriteExperimentHierarchyJson(std::ostream& output,
                                  const UnifiedExperimentSummary& summary,
                                  const ExperimentJsonSections& sections);
```

Move generic root ordering, entity-inventory writing, and callback invocation into new `statistics/json/document.cc`. The generic overload validates both callbacks, writes the existing root order, and calls them at the existing two positions. Keep `ExperimentStatistics` file I/O, current measurement semantics, and the `ScenarioConfig` overload in `writer.cc`; that overload constructs callbacks reproducing current output byte-for-byte. This lets the benchmark link the generic document writer without pulling agent-aware `ExperimentStatistics` ownership into its target.

Add `document.cc` exactly once to root test sources and the existing `llm-scenario` source list.

- [ ] **Step 6: Update Python exact category validation**

Require all four keys in every emitted `phy_stats`. Accept null or finite numeric rates. Accept null or finite fractions in `[0,1]`. Do not alter the eight validation keys or any directional category.

- [ ] **Step 7: Run GREEN regression**

```bash
./ns3 build llm-test llm-scenario
./test.py -s llm --no-build
./test.py -e 'llm-scenario*' --no-build
PYTHONDONTWRITEBYTECODE=1 python3 contrib/llm/scripts/live_test_traces.py --self-test
./utils/check-style-clang-format.py contrib/llm/examples/statistics contrib/llm/test/statistics
git -C contrib/llm diff --check
```

Expected: existing scenario/schema behavior passes; new fields are present/null unless explicitly set.

- [ ] **Step 8: Commit**

```bash
git add examples/statistics/output-types.h examples/statistics/json/phy.cc \
  examples/statistics/json/writer.h examples/statistics/json/writer.cc \
  examples/statistics/json/document.cc CMakeLists.txt examples/CMakeLists.txt \
  test/statistics/experiment-hierarchy-json-test-suite.cc \
  test/statistics/experiment-json-test-suite.cc \
  scripts/live_verification/schema_categories.py \
  scripts/tests/live_verification/fixtures.py \
  scripts/tests/live_verification/test_schema_root.py
git commit -m "llm: Add station PHY output fields"
```

---

### Task 2: Add saturated benchmark configuration

**Files:**
- Create: `config/saturated_tcp_config.toml`
- Create: `examples/saturated-tcp/config.h`
- Create: `examples/saturated-tcp/config-internal.h`
- Create: `examples/saturated-tcp/config.cc`
- Create: `examples/saturated-tcp/config-toml.cc`
- Create: `examples/saturated-tcp/config-cli.cc`
- Create: `examples/saturated-tcp/config-validation.cc`
- Create: `examples/saturated-tcp/config-json.cc`
- Create: `test/saturated-tcp/config-test-suite.cc`
- Modify: `CMakeLists.txt`
- Modify: `test/llm-test-suite.h`
- Modify: `test/llm-test-suite.cc`

**Interfaces:**
- Produces enums `SaturatedRssiRange`, `SaturatedInterferenceMode`, `SaturatedTrafficMode`, `SaturatedMimoMode`.
- Produces `SaturatedTcpConfig ParseSaturatedTcpConfig(int argc, char** argv)` and `WriteEffectiveSaturatedTcpConfigurationJson(JsonWriter&, const SaturatedTcpConfig&)`.
- Produces exact lower-case spellings `high|medium|low`, `isolated|ap_only_cochannel`, `ul|dl|ul_dl`, `su|mu` where `mu` validation always rejects current support.

- [ ] **Step 1: Define failing config tests**

Register `CreateSaturatedTcpConfigTestCases()` in the single llm suite. Tests must cover:

```cpp
config.script.repetitions == 1;
config.simulation.rngSeed == 12345;
config.benchmark.stationCountPerBss == 5;
config.wifi.channelNumber == 42;
config.wifi.bandwidthMhz == 80;
config.wifi.txPowerDbm == 20.0;
config.wifi.rateManager == "ns3::MinstrelHtWifiManager";
config.tcp.congestionControl == "ns3::TcpHighSpeed";
config.statistics.windowMs == 10;
```

Add TOML+CLI precedence cases, exact enum round trips, every boundary, malformed duration/rate strings, `window_ms` not dividing 1000, zero repetitions, and `mimo_mode=mu` rejection containing `DL MU-MIMO is not supported`.

- [ ] **Step 2: Run RED build**

```bash
./ns3 build llm-test
```

Expected: fails because the config types/factory are absent.

- [ ] **Step 3: Implement exact configuration types**

Use focused nested structs:

```cpp
struct SaturatedScriptConfig { uint32_t repetitions{1}; };
struct SaturatedSimulationConfig { uint32_t rngSeed{12345}; uint64_t rngRun{1}; };
struct SaturatedBenchmarkConfig
{
    uint32_t stationCountPerBss{5};
    SaturatedRssiRange rssiRange{SaturatedRssiRange::HIGH};
    SaturatedInterferenceMode interferenceMode{SaturatedInterferenceMode::ISOLATED};
    SaturatedTrafficMode trafficMode{SaturatedTrafficMode::UL};
    SaturatedMimoMode mimoMode{SaturatedMimoMode::SU};
};
```

Add corresponding general, Wi-Fi, TCP, statistics, and logging structs with the spec defaults. Reuse existing TOML/CLI parsing patterns but maintain a separate option registry and diagnostics prefixed with saturated config paths/flags.

- [ ] **Step 4: Implement TOML, CLI, validation, and JSON metadata**

Load the explicit config path first, then apply CLI overrides. Validate all spec constraints, registered TypeIds, simple output names, optional run folder, positive finite values, and exact matrix enums. `WriteEffectiveSaturatedTcpConfigurationJson` preserves TOML section/key order and scalar types.

- [ ] **Step 5: Run GREEN config tests and style**

```bash
./ns3 build llm-test
./test.py -s llm --no-build
./utils/check-style-clang-format.py contrib/llm/examples/saturated-tcp \
  contrib/llm/test/saturated-tcp
git -C contrib/llm diff --check
```

- [ ] **Step 6: Commit**

```bash
git add config/saturated_tcp_config.toml examples/saturated-tcp \
  test/saturated-tcp/config-test-suite.cc CMakeLists.txt \
  test/llm-test-suite.h test/llm-test-suite.cc
git commit -m "llm: Add saturated benchmark configuration"
```

---

### Task 3: Implement native propagation filtering and RSSI placement

**Files:**
- Create: `examples/saturated-tcp/bss-link-filter.h`
- Create: `examples/saturated-tcp/bss-link-filter.cc`
- Create: `examples/saturated-tcp/rssi-placement.h`
- Create: `examples/saturated-tcp/rssi-placement.cc`
- Create: `test/saturated-tcp/propagation-test-suite.cc`
- Modify: `CMakeLists.txt`
- Modify: `test/llm-test-suite.h`
- Modify: `test/llm-test-suite.cc`

**Interfaces:**
- Produces `enum class SaturatedRadioRole { ACCESS_POINT, STATION }`.
- Produces `BssLinkFilterPropagationLossModel`, delegating allowed links to a supplied native `PropagationLossModel`.
- Produces these exact placement functions:

```cpp
double SolveDistanceForRssi(Ptr<PropagationLossModel> lossModel,
                            double txPowerDbm,
                            double targetRssiDbm);
std::array<Vector, 3> BuildAccessPointTriangle(double sideLengthM);
std::vector<Vector> BuildStationRing(const Vector& accessPointPosition,
                                     double radiusM,
                                     uint32_t stationCount,
                                     double angularOffsetRadians);
```

- [ ] **Step 1: Write failing link-policy tests**

Register `CreateSaturatedTcpPropagationTestCases()`. Build six mobility objects (AP/STA in BSS 0/1/2), register identities, and assert:

```text
same-BSS AP/STA, STA/STA, AP/AP -> native result
cross-BSS AP/AP -> native result
cross-BSS AP/STA, STA/AP, STA/STA -> -infinity
unregistered mobility -> throws with node/mobility identity
```

Use a deterministic fake native loss model that returns `txPowerDbm - distance` so delegation is directly observable.

- [ ] **Step 2: Write failing placement tests**

Against native `LogDistancePropagationLossModel`, solve distances for -41.5, -50, and -60 dBm at 20 dBm TX. Assert recalculated RSSI is within 0.5 dB, distance increases as target weakens, triangle sides are equal, every ring point has the solved radius, angles are equal, and BSS offsets avoid identical coordinates.

- [ ] **Step 3: Run RED build**

```bash
./ns3 build llm-test
```

Expected: fails because filter/placement APIs are absent.

- [ ] **Step 4: Implement the filter**

Define:

```cpp
class BssLinkFilterPropagationLossModel : public PropagationLossModel
{
  public:
    static TypeId GetTypeId();
    void SetNativeLossModel(Ptr<PropagationLossModel> nativeLoss);
    void RegisterRadio(Ptr<MobilityModel> mobility,
                       uint32_t bssId,
                       SaturatedRadioRole role);

  private:
    double DoCalcRxPower(double txPowerDbm,
                         Ptr<MobilityModel> sender,
                         Ptr<MobilityModel> receiver) const override;
    int64_t DoAssignStreams(int64_t stream) override;
};
```

Same-BSS links delegate. Cross-BSS links delegate only when both roles are AP. Every cross-BSS link involving a station returns negative infinity. Validate duplicate registrations and missing native model.

- [ ] **Step 5: Implement monotonic distance solving and coordinates**

Use binary search over a positive bracket, evaluate the concrete native model through two `ConstantPositionMobilityModel` objects, stop when RSSI error is <=0.01 dB, and fail if no bracket/monotonic solution exists. Triangle coordinates are `(0,0,0)`, `(d,0,0)`, and `(d/2,sqrt(3)d/2,0)`. Ring point `i` uses `2*pi*i/count + bssOffset`.

- [ ] **Step 6: Run GREEN tests**

```bash
./ns3 build llm-test
./test.py -s llm --no-build
./utils/check-style-clang-format.py contrib/llm/examples/saturated-tcp \
  contrib/llm/test/saturated-tcp
```

- [ ] **Step 7: Commit**

```bash
git add examples/saturated-tcp/bss-link-filter.* \
  examples/saturated-tcp/rssi-placement.* \
  test/saturated-tcp/propagation-test-suite.cc CMakeLists.txt \
  test/llm-test-suite.h test/llm-test-suite.cc
git commit -m "llm: Add benchmark propagation policy"
```

---

### Task 4: Track STA EDCA access waiting without core changes

**Files:**
- Create: `examples/saturated-tcp/access-tracking-sta-wifi-mac.h`
- Create: `examples/saturated-tcp/access-tracking-sta-wifi-mac.cc`
- Create: `examples/saturated-tcp/access-wait-tracker.h`
- Create: `examples/saturated-tcp/access-wait-tracker.cc`
- Create: `test/saturated-tcp/access-wait-test-suite.cc`
- Modify: `CMakeLists.txt`
- Modify: `test/llm-test-suite.h`
- Modify: `test/llm-test-suite.cc`

**Interfaces:**
- Produces TypeId `ns3::AccessTrackingStaWifiMac`, derived from `StaWifiMac` with `AccessRequested` trace `(uint8_t ac, uint8_t linkId)`.
- Produces `AccessWaitTracker::NotifyRequest`, `NotifyGrant`, `Finalize`, and `GetUnionIntervals` in nanoseconds.
- Consumes existing per-AC `QosTxop::TxopTrace(start,duration,linkId)` to recover actual access-grant start.

- [ ] **Step 1: Add failing STA MAC trace tests**

Create an `AccessTrackingStaWifiMac`, attach `QosTxop` objects, connect `AccessRequested`, call `NotifyRequestAccess()` through a test seam, and assert the emitted AC/link plus unchanged base behavior. Assert duplicate registration/reporting is not synthesized by the subclass.

- [ ] **Step 2: Add failing interval-union tests**

Exercise exact nanosecond sequences:

```text
BE request 100 -> grant 300                  => [100,300]
BE request 400 + VI request 450
BE grant 500 + VI grant 700                  => [400,700]
duplicate BE request before grant            => one pending interval
request 800 + finalize 1000                  => [800,1000]
grant without pending request                => ignored TXOP continuation
```

Assert clipped measurement/window overlap produces no interval outside the measurement epoch.

- [ ] **Step 3: Run RED build**

```bash
./ns3 build llm-test
```

- [ ] **Step 4: Implement the STA MAC trace seam**

Override:

```cpp
void NotifyRequestAccess(Ptr<Txop> txop, uint8_t linkId) override;
```

Resolve the access category from `QosTxop::GetAccessCategory()` (or `AC_BE_NQOS`), emit one trace event, then call `StaWifiMac::NotifyRequestAccess(txop, linkId)`. The subclass must add no scheduling or MAC-policy change.

- [ ] **Step 5: Implement historical access intervals**

`AccessWaitTracker` stores the first request time per `(AC,link)`. A TXOP trace received at release supplies its historical start time; that start closes the matching pending access. Store raw intervals per AC, then union/sort them at finalization so callback delivery time cannot distort the wait. Pending requests close at measurement end. Clip all intervals to `[epoch, epoch+1s)`.

- [ ] **Step 6: Run GREEN tests and style**

```bash
./ns3 build llm-test
./test.py -s llm --no-build
./utils/check-style-clang-format.py contrib/llm/examples/saturated-tcp \
  contrib/llm/test/saturated-tcp
```

- [ ] **Step 7: Commit**

```bash
git add examples/saturated-tcp/access-tracking-sta-wifi-mac.* \
  examples/saturated-tcp/access-wait-tracker.* \
  test/saturated-tcp/access-wait-test-suite.cc CMakeLists.txt \
  test/llm-test-suite.h test/llm-test-suite.cc
git commit -m "llm: Track station channel access waiting"
```

---

### Task 5: Collect station-transmitted PPDU metrics by window

**Files:**
- Create: `examples/saturated-tcp/sta-phy-metrics.h`
- Create: `examples/saturated-tcp/sta-phy-metrics-internal.h`
- Create: `examples/saturated-tcp/sta-phy-metrics.cc`
- Create: `test/saturated-tcp/sta-phy-metrics-test-suite.cc`
- Modify: `CMakeLists.txt`
- Modify: `test/llm-test-suite.h`
- Modify: `test/llm-test-suite.cc`

**Interfaces:**
- Produces raw `StationPhyMetricAccumulator` with nominal-rate-times-airtime, PSDU bits, PPDU airtime, and contention nanoseconds.
- Produces `StationPhyMetricOutput DeriveStationPhyMetrics(const StationPhyMetricAccumulator&, int64_t windowDurationNs)`.
- Produces recorder methods for PPDU attempts and contention intervals split across fixed 10 ms windows.

Use these exact value types:

```cpp
struct StationPhyMetricAccumulator
{
    long double nominalRateBpsNs{0.0L};
    long double psduBits{0.0L};
    int64_t ppduAirtimeNs{0};
    int64_t contentionNs{0};
    void Merge(const StationPhyMetricAccumulator& other);
};

struct StationPhyMetricOutput
{
    std::optional<double> averageTheoreticalPhyRateMbps;
    std::optional<double> averagePracticalPhyRateMbps;
    std::optional<double> channelEfficiency;
    std::optional<double> contentionFraction;
};
```

- [ ] **Step 1: Write failing formula tests**

For two PPDUs, use:

```text
PPDU A: nominal 100 Mbps, 100 us, 1000 PSDU bytes
PPDU B: nominal 200 Mbps, 300 us, 6000 PSDU bytes
contention: 100 us in a 1 ms window
```

Assert:

```text
theoretical = 175 Mbps
practical = 140 Mbps
efficiency = 0.8
contention = 0.1
```

Assert zero airtime yields null rates/efficiency and valid contention. Assert practical above theoretical beyond tolerance is rejected.

- [ ] **Step 2: Write failing PPDU classification tests**

Build `WifiPsdu` fixtures for unicast data, TCP-ACK-sized data, MAC ACK/control, BlockAck, RTS/CTS, retransmission copies, beacon, association/probe, broadcast, and unrelated frames. Assert only spec-qualified station transmissions contribute and retransmissions contribute again.

- [ ] **Step 3: Write failing window-splitting tests**

Record a PPDU and contention interval crossing a 10 ms boundary. Allocate airtime, nominal-rate product, and PSDU bits proportionally by nanosecond overlap. Assert window totals merge back exactly (within long-double tolerance) to `overall` raw totals.

- [ ] **Step 4: Run RED build**

```bash
./ns3 build llm-test
```

- [ ] **Step 5: Implement PPDU extraction and nanosecond accumulators**

On `PhyTxPsduBegin`, calculate duration with `WifiPhy::CalculateTxDuration`, obtain each user's nominal `WifiTxVector` mode rate, sum qualifying MPDU/PSDU sizes, and record only on registered station devices. Use `long double` for rate-time and proportional bit allocations. Do not use `AppTxTag`.

- [ ] **Step 6: Implement formulas and access-wait ingestion**

Derive exactly the spec formulas. `channelEfficiency` and `contentionFraction` are optional only when their denominator is zero; clamp values within floating-point tolerance at 0/1 and reject material violations. Ingest union intervals from Task 4 and split them over windows in nanoseconds.

- [ ] **Step 7: Run GREEN tests**

```bash
./ns3 build llm-test
./test.py -s llm --no-build
./utils/check-style-clang-format.py contrib/llm/examples/saturated-tcp \
  contrib/llm/test/saturated-tcp
```

- [ ] **Step 8: Commit**

```bash
git add examples/saturated-tcp/sta-phy-metrics* \
  test/saturated-tcp/sta-phy-metrics-test-suite.cc CMakeLists.txt \
  test/llm-test-suite.h test/llm-test-suite.cc
git commit -m "llm: Collect station PPDU metrics"
```

---

### Task 6: Build benchmark windows, BSS summaries, and shared JSON

**Files:**
- Create: `examples/saturated-tcp/benchmark-statistics.h`
- Create: `examples/saturated-tcp/benchmark-statistics.cc`
- Create: `examples/saturated-tcp/output.cc`
- Create: `test/saturated-tcp/benchmark-output-test-suite.cc`
- Modify: `CMakeLists.txt`
- Modify: `test/llm-test-suite.h`
- Modify: `test/llm-test-suite.cc`

**Interfaces:**
- Produces `SaturatedTcpStatistics` registration/connect/start/finalize API.
- Produces `UnifiedExperimentSummary BuildSummary() const` using the shared AP/STA DTO hierarchy.
- Produces `WriteSaturatedTcpExperimentJson(path, summary, config)` through Task 1 generic JSON sections.

- [ ] **Step 1: Write failing station/BSS summary tests**

Register three BSSs and stations with literal raw windows. Assert station DTO fields copy derived values. Assert AP PHY fields are:

```text
theoretical = arithmetic mean of defined station theoretical rates
practical = arithmetic mean of defined station practical rates
efficiency = AP practical / AP theoretical
contention = arithmetic mean of every station contention fraction
```

Short-window null station rates are excluded from AP rate means. The original Task 6 acceptance assumed every saturated `overall` station rate would be defined; the Task 11 evidence amendment below supersedes that assumption without rewriting its history. Assert AP-originated raw observations cannot enter these fields.

- [ ] **Step 2: Write failing JSON integration tests**

Create one literal 10 ms window plus overall, benchmark config, and inventory. Assert the root order is unchanged, benchmark measurement semantics describe station-transmitted PPDUs, configuration has benchmark sections, AP/STA fields are exact, other categories retain fixed shapes, and output ends with one newline. Add collision/missing-parent/rejecting-stream tests equivalent to the existing writer lifecycle.

- [ ] **Step 3: Run RED build**

```bash
./ns3 build llm-test
```

- [ ] **Step 4: Implement benchmark statistics ownership**

Define:

```cpp
class SaturatedTcpStatistics
{
  public:
    explicit SaturatedTcpStatistics(uint32_t windowMs);
    void RegisterAccessPoint(uint32_t accessPointId,
                             uint32_t nodeId,
                             std::string nodeLabel,
                             std::string ipv4);
    void RegisterStation(uint32_t accessPointId,
                         uint32_t stationIndex,
                         uint32_t nodeId,
                         std::string nodeLabel,
                         std::string ipv4);
    void ConnectStation(Ptr<WifiNetDevice> device);
    void Start(int64_t experimentStartNs);
    void Finalize(int64_t experimentEndNs);
    UnifiedExperimentSummary BuildSummary() const;
};
```

Registration uses `ExperimentEntityRegistry`. `ConnectStation` attaches Task 4 request/TXOP traces and Task 5 `PhyTxPsduBegin`. Callbacks before `Start` do not contribute. Finalize closes pending waits at the exact one-second end and is idempotent.

- [ ] **Step 5: Build sparse windows and dense overall from raw state**

Use exactly 100 10 ms windows by default. Emit a station/window only when it has PPDU or contention activity. Emit AP/window when at least one child appears. Build overall by merging raw per-station accumulators, then derive values once. Set existing eight validation flags through the same fixed output shape, with vacuous unrelated-category checks true and overall/window/inventory checks real.

- [ ] **Step 6: Implement benchmark metadata and file output**

Use `ExperimentJsonSections` callbacks:

```cpp
sections.writeMeasurementSemantics = WriteSaturatedMeasurementSemantics;
sections.writeConfiguration = [&config](JsonWriter& writer) {
    WriteEffectiveSaturatedTcpConfigurationJson(writer, config);
};
```

Write exclusively with `std::ios::noreplace` and preserve path-bearing open/write/flush/close errors.

- [ ] **Step 7: Run GREEN tests**

```bash
./ns3 build llm-test
./test.py -s llm --no-build
./utils/check-style-clang-format.py contrib/llm/examples/saturated-tcp \
  contrib/llm/test/saturated-tcp
```

- [ ] **Step 8: Commit**

```bash
git add examples/saturated-tcp/benchmark-statistics.* \
  examples/saturated-tcp/output.cc \
  test/saturated-tcp/benchmark-output-test-suite.cc CMakeLists.txt \
  test/llm-test-suite.h test/llm-test-suite.cc
git commit -m "llm: Build saturated benchmark summaries"
```

---

### Task 7: Add readiness-gated saturated TCP senders

**Files:**
- Create: `examples/saturated-tcp/saturated-tcp-sender.h`
- Create: `examples/saturated-tcp/saturated-tcp-sender.cc`
- Create: `examples/saturated-tcp/readiness-barrier.h`
- Create: `examples/saturated-tcp/readiness-barrier.cc`
- Create: `examples/saturated-tcp/traffic.h`
- Create: `examples/saturated-tcp/traffic.cc`
- Create: `test/saturated-tcp/traffic-test-suite.cc`
- Modify: `CMakeLists.txt`
- Modify: `test/llm-test-suite.h`
- Modify: `test/llm-test-suite.cc`

**Interfaces:**
- Produces `SaturatedTcpSender`, an unlimited TCP source with `SetReadyCallback()` and `StartTraffic()`.
- Produces `SaturatedReadinessBarrier` that opens on the next whole second after all senders connect and starts statistics/senders together.
- Produces flow installation for UL, DL, and UL+DL using separate ports.

- [ ] **Step 1: Write failing sender state tests**

Test exact transitions:

```text
application start -> socket connect begins
connection success -> ready callback once, zero payload sent
StartTraffic before ready -> abort
StartTraffic twice -> abort
StartTraffic after ready -> send until buffer full
send callback -> resume unlimited sending
stop -> close socket and cancel callbacks
connection failure -> clear fatal diagnostic
```

Use a real two-node point-to-point TCP fixture for connection/readiness and a bounded simulation stop; assert no payload reaches the sink before `StartTraffic()`.

- [ ] **Step 2: Write failing barrier tests**

With fake sender/statistics callbacks, report readiness at 1.10, 1.25, and 1.37 seconds. Assert common epoch 2.00 seconds, all starts exactly once there, statistics end 3.00 seconds, simulator stop immediately after, duplicate readiness rejected, zero registrations rejected, and a fixed 30-second simulation-time safety timeout without readiness reports failure.

- [ ] **Step 3: Write failing flow-matrix tests**

For N stations per BSS, assert sender counts:

```text
UL: N station senders
DL: N server senders
UL+DL: 2*N independent senders
```

Assert one sink per destination/port, no shared TCP connection, all three BSSs use the same mode, and each sender is registered with the barrier.

- [ ] **Step 4: Run RED build**

```bash
./ns3 build llm-test
```

- [ ] **Step 5: Implement the sender**

Derive from `SourceApplication`. Create/bind/connect the TCP socket in `DoStartApplication`, but do not call `Send()` from the connection-success callback. `StartTraffic()` enables the same loop as unlimited BulkSend: create `sendSize` packets until `Socket::Send()` returns full-buffer failure, retain an unsent packet, and continue from the socket send callback. Emit ready once and expose no MaxBytes/DataRate throttle.

- [ ] **Step 6: Implement barrier and flow installation**

The barrier owns registered sender/application pointers plus callbacks to start/finalize `SaturatedTcpStatistics`. After the final ready callback, calculate the next whole second locally as `((nowNs / 1s) + 1) * 1s`; do not link or depend on the agent-aware `TrafficCoordinator`. Schedule the common epoch, set application stop times, and install a 30-second simulation-time pre-epoch safety stop that is cancelled when the barrier opens. Install `PacketSink` endpoints and dedicated source ports deterministically.

- [ ] **Step 7: Run GREEN tests**

```bash
./ns3 build llm-test
./test.py -s llm --no-build
./utils/check-style-clang-format.py contrib/llm/examples/saturated-tcp \
  contrib/llm/test/saturated-tcp
```

- [ ] **Step 8: Commit**

```bash
git add examples/saturated-tcp/saturated-tcp-sender.* \
  examples/saturated-tcp/readiness-barrier.* examples/saturated-tcp/traffic.* \
  test/saturated-tcp/traffic-test-suite.cc CMakeLists.txt \
  test/llm-test-suite.h test/llm-test-suite.cc
git commit -m "llm: Add readiness-gated saturated traffic"
```

---

### Task 8: Build the routed three-BSS scenario executable

**Files:**
- Create: `examples/saturated-tcp/topology.h`
- Create: `examples/saturated-tcp/topology.cc`
- Create: `examples/saturated-tcp/log.h`
- Create: `examples/saturated-tcp/log.cc`
- Create: `examples/saturated-tcp-scenario.cc`
- Create: `test/saturated-tcp/topology-test-suite.cc`
- Modify: `examples/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Modify: `test/examples-to-run.py`
- Modify: `test/llm-test-suite.h`
- Modify: `test/llm-test-suite.cc`

**Interfaces:**
- Produces example target `saturated-tcp-scenario`.
- Produces `SaturatedTcpTopology BuildSaturatedTcpTopology(const SaturatedTcpConfig&)` containing exactly three BSSs, dedicated servers, AP/station devices, interfaces, positions, and identities.
- Consumes all Tasks 2-7 and writes one shared-schema `output.json`.

The topology result has this stable shape:

```cpp
struct SaturatedTcpBssTopology
{
    uint32_t bssId;
    Ptr<Node> serverNode;
    Ptr<Node> accessPointNode;
    NodeContainer stationNodes;
    Ptr<WifiNetDevice> accessPointDevice;
    NetDeviceContainer stationDevices;
    Ipv4InterfaceContainer stationInterfaces;
    Ipv4Address serverAddress;
};

struct SaturatedTcpTopology
{
    std::array<SaturatedTcpBssTopology, 3> bss;
    double accessPointDistanceM;
    double stationDistanceM;
};
```

- [ ] **Step 1: Write failing topology attribute tests**

Build a one-station topology fixture and assert:

```text
3 APs, 3 servers, 3*N STAs
one 10Gbps/0.1ms p2p link per AP/server
5GHz channel 42, 80MHz, primary20=0
20dBm fixed TX power
MinstrelHtWifiManager
2 antennas and 2 max TX/RX streams on APs and STAs
BSS colors 1,2,3
custom AccessTrackingStaWifiMac on every STA
SU mode accepted, MU rejected
```

- [ ] **Step 2: Write failing isolated/co-channel live topology tests**

For isolated mode, assert three different channel pointers and no cross-BSS delivery. For co-channel mode, assert one channel pointer, native AP/AP RSSI near -50 dBm, same-BSS target RSSI, and the full allowed/blocked link matrix. Assert all routes and server/STA addresses are reachable within their own BSS.

- [ ] **Step 3: Add the registered smoke command before target exists**

Register a one-station, high-RSSI, isolated, UL, SU run with an explicit temporary run folder and output name. Run:

```bash
./test.py -e 'saturated-tcp-scenario*' --no-build
```

Expected: no target/example exists yet.

- [ ] **Step 4: Implement topology construction**

Create the native loss model, solve AP/STA distances, place fixed nodes, create shared filtered or three isolated channels, install AP/STA Wi-Fi with exact attributes/colors, install Internet stack and point-to-point server links, assign deterministic subnets, populate routes, register radios with the filter, and validate every required RSSI before returning.

- [ ] **Step 5: Implement scenario orchestration**

Parse config, set RNG seed/run and TCP defaults before stack/socket creation, build topology, register/connect benchmark statistics, install sinks/senders for all BSSs, finalize the barrier, run simulation, require a completed measurement epoch, build/validate summary, resolve/create run path, and write one exclusive output JSON. Console output states exact config, solved distances/RSSI, readiness epoch, and output path without dumping metric reports.

- [ ] **Step 6: Wire CMake target and tests**

Add every benchmark source once to root `test_sources` and to the new example `SOURCE_FILES`. The example also compiles shared JSON `statistics/json/document.cc`, `hierarchy.cc`, `general.cc`, `app.cc`, `tcp.cc`, `mac.cc`, `phy.cc`, and `entity.cc`; it does not compile `statistics/json/writer.cc` or current agent-aware statistics ownership. Link existing `${libllm}` and required ns-3 modules only. Keep `examples/` root with both entry sources plus CMake. Register the smoke command against `config/saturated_tcp_config.toml` with CLI overrides for one STA and a 1-second measurement.

- [ ] **Step 7: Run GREEN end-to-end smoke**

```bash
./ns3 build llm-test saturated-tcp-scenario
./test.py -s llm --no-build
./test.py -e 'saturated-tcp-scenario*' --no-build
./utils/check-style-clang-format.py contrib/llm/examples/saturated-tcp \
  contrib/llm/examples/saturated-tcp-scenario.cc contrib/llm/test/saturated-tcp
```

Parse the smoke JSON and assert root schema, exactly 100 windows at 10 ms, 3 APs, 3 STAs, non-null overall metrics, target RSSI metadata, all eight validation flags true, and no output leftover after test cleanup.

- [ ] **Step 8: Commit**

```bash
git add examples/saturated-tcp/topology.* examples/saturated-tcp/log.* \
  examples/saturated-tcp-scenario.cc examples/CMakeLists.txt CMakeLists.txt \
  test/saturated-tcp/topology-test-suite.cc test/examples-to-run.py \
  test/llm-test-suite.h test/llm-test-suite.cc
git commit -m "llm: Add saturated TCP scenario"
```

---

### Task 9: Implement the sequential matrix runner and Excel CSV

**Files:**
- Create: `exp_scripts/__init__.py`
- Create: `exp_scripts/saturated_tcp_experiment.py`
- Create: `exp_scripts/saturated_tcp_benchmark/__init__.py`
- Create: `exp_scripts/saturated_tcp_benchmark/matrix.py`
- Create: `exp_scripts/saturated_tcp_benchmark/csv_output.py`
- Create: `exp_scripts/saturated_tcp_benchmark/validation.py`
- Create: `exp_scripts/saturated_tcp_benchmark/runner.py`
- Create: `exp_scripts/tests/__init__.py`
- Create: `exp_scripts/tests/test_saturated_tcp_matrix.py`
- Create: `exp_scripts/tests/test_saturated_tcp_csv.py`
- Create: `exp_scripts/tests/test_saturated_tcp_validation.py`
- Create: `exp_scripts/tests/test_saturated_tcp_runner.py`

**Interfaces:**
- Produces 108 ordered `ExperimentConfiguration` objects for SU.
- Produces one subprocess command/run directory per `(configuration,repetition_attempt)`.
- Produces exactly three validated `BssCsvRow` values per successful JSON.
- Produces one fixed-width Excel-compatible `results.csv`.

Use these immutable Python boundaries:

```python
@dataclass(frozen=True)
class ExperimentConfiguration:
    experiment_id: int
    sta_count_per_bss: int
    rssi_range: str
    interference_mode: str
    traffic_mode: str
    mimo_mode: str

@dataclass(frozen=True)
class StationCsvMetrics:
    average_theoretical_phy_rate_mbps: float
    average_practical_phy_rate_mbps: float
    efficiency: float
    contention_fraction: float

@dataclass(frozen=True)
class BssCsvRow:
    configuration: ExperimentConfiguration
    repetition_attempt: int
    target_rssi_dbm: float
    bss_id: int
    average_theoretical_phy_rate_mbps: float
    average_practical_phy_rate_mbps: float
    efficiency: float
    contention_fraction: float
    stations: tuple[StationCsvMetrics | None, ...]  # exactly 30 entries
```

- [ ] **Step 1: Write failing exact matrix tests**

Assert nested order:

```python
STA_COUNTS = (5, 10, 15, 20, 25, 30)
RSSI_RANGES = ("high", "medium", "low")
INTERFERENCE_MODES = ("isolated", "ap_only_cochannel")
TRAFFIC_MODES = ("ul", "dl", "ul_dl")
MIMO_MODES = ("su",)
```

Assert length 108, `experiment_id` 1 through 108, default repetitions one, attempts do not change experiment ID, `rng_run == repetition_attempt`, and `mu` is absent/rejected.

- [ ] **Step 2: Write failing exact CSV-format tests**

Build the header programmatically but assert byte-for-byte against the approved sequence: nine identity columns, four BSS columns, then four columns for each station 0 through 29. Write one row and assert:

```text
file begins EF BB BF
delimiter is ;
line endings are CRLF only
decimal is 41.5, never 41,5
quotes follow csv.QUOTE_MINIMAL
unused station cells are empty
```

Assert no TCP-goodput or diagnostic column exists.

- [ ] **Step 3: Write failing JSON validation/copy tests**

Use a complete shared-schema fixture with 3 APs and N stations. The original Task 9 fixture asserted non-null overall station fields; the Task 11 evidence amendment below adds the approved paired-null station/BSS cases while retaining exact metadata, inventory, formulas, and CSV copying. Reject duplicate/missing BSS/stations, non-finite values, practical above theoretical, wrong efficiency, contention outside `[0,1]`, and nonempty columns for nonexistent stations.

- [ ] **Step 4: Write failing runner lifecycle tests**

Inject a fake subprocess function. Assert one process at a time, exact command flags, deterministic paths, exclusive timestamp folder/CSV, a 600-second wall timeout, three rows appended only after validation, JSON retained, nonzero/timeout/missing/invalid output stops immediately, failed attempt adds zero rows, completed rows survive, SIGINT exits nonzero, and existing paths are never overwritten.

- [ ] **Step 5: Run RED Python tests**

```bash
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover \
  -s contrib/llm/exp_scripts/tests -t contrib/llm/exp_scripts -p 'test_*.py' -v
```

Expected: import failures because runner modules do not exist.

- [ ] **Step 6: Implement matrix and validation modules**

Use frozen dataclasses for configurations/rows in the `saturated_tcp_benchmark` package. `target_rssi_dbm` maps high=-41.5, medium=-50.0, low=-60.0. `validate_output_document()` checks the shared root plus benchmark-specific config/inventory/formulas and returns BSS rows in BSS ID order. Use only Python standard library.

- [ ] **Step 7: Implement CSV writer**

Open with `encoding="utf-8-sig"`, `newline=""`, `delimiter=";"`, `lineterminator="\r\n"`, and `quoting=csv.QUOTE_MINIMAL`. Create exclusively. Append all three rows for one attempt as one prepared text block, flush, and `os.fsync()` before the next subprocess.

- [ ] **Step 8: Implement sequential CLI entry point**

The thin root entry script imports `saturated_tcp_benchmark.runner` directly; executing the file places `exp_scripts/` on `sys.path`, so no path mutation is required. Default config is `contrib/llm/config/saturated_tcp_config.toml`; default ns-3 root is discovered from the script path and must equal the current working project when launched from outer root. Parse repetitions with `tomllib`. Create `run/scripted_exp_<timestamp>`, then invoke with `timeout=600` seconds:

```python
[str(ns3), "run", command_string]
```

The command string uses target `saturated-tcp-scenario`, the config path, exact benchmark overrides, `--simulation-rng-seed=12345`, `--simulation-rng-run=<attempt>`, exact run folder, and `output.json`. Capture stdout/stderr for error diagnostics without creating another CSV.

- [ ] **Step 9: Run GREEN tests and direct help**

```bash
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover \
  -s contrib/llm/exp_scripts/tests -t contrib/llm/exp_scripts -p 'test_*.py' -v
PYTHONDONTWRITEBYTECODE=1 python3 contrib/llm/exp_scripts/saturated_tcp_experiment.py --help
test -z "$(find contrib/llm/exp_scripts -type d -name __pycache__ -print -quit)"
git -C contrib/llm diff --check
```

- [ ] **Step 10: Commit**

```bash
git add exp_scripts
git commit -m "llm: Add saturated benchmark runner"
```

---

### Task 10: Document and deterministically verify the complete benchmark

**Files:**
- Modify: `README.md`
- Modify: `README_RU.md`
- Modify: `test/examples-to-run.py` if smoke wording/path needs correction
- Modify only if a deterministic defect is found: files owned by Tasks 1-9

**Interfaces:**
- Consumes: complete scenario and runner.
- Produces: synchronized EN/RU benchmark documentation and green deterministic verification before the expensive matrix.

- [ ] **Step 1: Document benchmark purpose and limitation**

In both READMEs, explain station-only semantics, custom cross-BSS visibility assumption, native propagation, 2x2 SU, current MU-MIMO limitation, readiness epoch, formulas, null windows, BSS aggregation, JSON fields, fixed CSV columns/Excel encoding, output tree, repetitions, and the exact 108-run count. Do not claim TCP-goodput efficiency.

- [ ] **Step 2: Document exact commands**

Include:

```bash
./ns3 run "saturated-tcp-scenario --config contrib/llm/config/saturated_tcp_config.toml"
python3 contrib/llm/exp_scripts/saturated_tcp_experiment.py
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover \
  -s contrib/llm/exp_scripts/tests -t contrib/llm/exp_scripts -p 'test_*.py' -v
```

Update both project trees with `saturated-tcp-scenario.cc`, helper directory, config, runner modules, and tests.

- [ ] **Step 3: Run all deterministic verification**

From outer root:

```bash
PYTHONDONTWRITEBYTECODE=1 python3 contrib/llm/scripts/live_test_traces.py --self-test
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover \
  -s contrib/llm/scripts/tests -t contrib/llm/scripts -p 'test_*.py' -v
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover \
  -s contrib/llm/exp_scripts/tests -t contrib/llm/exp_scripts -p 'test_*.py' -v
./utils/check-style-clang-format.py contrib/llm/examples contrib/llm/model contrib/llm/test
./ns3 build llm-test llm-scenario saturated-tcp-scenario
./test.py -s llm --no-build
./test.py -e 'llm-scenario*' --no-build
./test.py -e 'saturated-tcp-scenario*' --no-build
git -C contrib/llm diff --check
```

Expected: all existing and new deterministic tests, targets, and both registered examples pass with pristine output.

- [ ] **Step 4: Run one direct five-STA scenario audit**

Use a newly created temporary outer run directory, high RSSI, isolated, UL, SU, RNG run 1. Parse output and assert 100 windows, 3 APs, 15 STAs, exact metadata, every overall metric non-null, BSS formulas, target RSSI validation, and all eight flags true. Remove only this temporary smoke directory after recording evidence.

- [ ] **Step 5: Check documentation parity and stale claims**

Require both READMEs to contain the same commands, field names, SU-only limitation, 108/324 counts, and no phrase defining efficiency as TCP goodput/PHY rate in the benchmark section. Check every changed relative link.

- [ ] **Step 6: Commit**

```bash
git add README.md README_RU.md test/examples-to-run.py
git commit -m "llm: Document saturated TCP benchmark"
```

If Step 3/4 required implementation corrections, commit those exact paths first with `llm: Fix saturated benchmark verification`, rerun Steps 3-5, then commit documentation separately.

---

### Task 11: Run and audit the full honest 108-scenario matrix

**Files:**
- Create outside nested Git only: `/home/bsa/projects/ns-3-dev/run/scripted_exp_<timestamp>/...`
- Modify only for concrete defects found by the full run: files owned by Tasks 1-10
- Do not add run JSON/CSV to Git

**Interfaces:**
- Consumes: final deterministic code and default one repetition.
- Produces: retained 108 JSON files, one 324-row CSV, and an evidence report with discrepancy analysis.

- [ ] **Step 1: Record pre-run preservation state**

Record nested/outer status, trace hashes/LFS state, outer `.gitignore` and `VAGUE_TASK.md` hashes, pre-existing run artifact hashes, disk free space, and the absence of a colliding timestamp directory. Confirm `script.repetitions = 1` and matrix length 108 through the tested Python API.

- [ ] **Step 2: Run the full matrix exactly once on the candidate code**

From `/home/bsa/projects/ns-3-dev`:

```bash
PYTHONDONTWRITEBYTECODE=1 python3 contrib/llm/exp_scripts/saturated_tcp_experiment.py
```

Do not pass a reduced matrix, duration, station count, or test flag. Capture wall time, final run directory, and per-run progress. Do not run matrix subprocesses manually in parallel.

- [ ] **Step 3: Verify structural completeness**

Assert exactly 108 experiment directories, one attempt each, 108 `output.json` files, one CSV, 324 data rows plus header, experiment IDs 1-108, three BSS rows per experiment, attempt 1, SU only, every matrix tuple once, and only valid empty station columns.

- [ ] **Step 4: Recompute every mathematical invariant independently**

Using a read-only Python audit over retained JSON/CSV, recompute station efficiency for defined rates, paired-null station semantics, AP/BSS defined-only rate means, nullable BSS efficiency, all-station numeric BSS contention, finite/range checks, practical<=theoretical tolerance, metadata/inventory/RSSI targets, and exact CSV blank-versus-zero values. Require zero structural/formula discrepancies.

- [ ] **Step 5: Analyze empirical trends without forcing them**

Group by all but one factor and inspect:

```text
high -> medium -> low target RSSI versus theoretical/practical rate
5 -> 30 STAs versus contention
isolated versus AP-only co-channel
UL versus DL versus UL+DL
three-BSS symmetry and outliers
Minstrel non-monotonic probes
```

Report every reversal/outlier with exact experiment/BSS/station IDs and values. Classify it as explainable Minstrel/MAC variation, suspicious, or structural. Do not rewrite or force data to match hypotheses.

- [ ] **Step 6: Handle discrepancies**

If a structural or code defect exists, stop accepting the dataset, dispatch/fix the minimal code in the normal review loop, rerun all deterministic gates, then run the full 108-scenario matrix again into a new timestamped directory. Retain the final accepted dataset; clearly label any earlier incomplete/invalid directory in the report and do not stage it.

- [ ] **Step 7: Final preservation and repository checks**

Assert no `__pycache__`, temporary experiment directory, unexpected `output.json`, or subprocess remains outside the retained final run. Recheck all pre-run hashes/status and nested `git diff --check`. The outer repo must retain only user-owned pre-existing changes plus the new authorized run directory.

- [ ] **Step 8: Commit only code fixes if needed**

If no defect was found, create no commit. If corrections were required, stage exact code/test/doc paths, never `run/`, and commit:

```bash
git commit -m "llm: Fix full benchmark discrepancies"
```

Rerun deterministic verification after the final code commit; do not rerun an already accepted final dataset when the last commit changes only documentation/evidence.

#### Task 11 evidence amendment: nullable one-second overall metrics

The first full-run attempts proved that TCP connection readiness does not
guarantee every STA transmits a qualifying PPDU during the following exact
one-second severe DL interval. The approved resolution keeps the interval and
the dense entity arrays unchanged:

- an overall STA with zero qualifying PPDU has JSON null theoretical rate,
  practical rate, and efficiency, plus numeric contention including `0.0`;
- BSS theoretical/practical means exclude undefined STA rates, BSS efficiency
  is the ratio of those means, and all configured STA contention values remain
  in the contention mean;
- a BSS with no defined STA rate has null theoretical rate, practical rate,
  and efficiency plus numeric contention;
- the 133-column CSV schema, UTF-8 BOM, semicolon delimiter, CRLF, and decimal
  dot stay fixed; undefined derived cells are empty while numeric contention
  zero is written as `0.0`;
- partial null triplets, nonnumeric contention, wrong formulas, and
  overall/window presence mismatches remain hard validation failures; and
- existing invalid run directories remain immutable evidence. A new focused
  experiment 071 and then the complete sequential 108-run matrix are required
  on the amended candidate.
