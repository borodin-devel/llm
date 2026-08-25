# Experiment JSON Output Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace split JSON/log summaries with one clear, unit-suffixed, streaming experiment JSON document ending in all 36 effective configuration values, and change the default filename to `output.json`.

**Architecture:** Measurement owners build typed transmission and cross-layer summaries without output side effects. A focused streaming serializer combines those summaries with sparse Wi-Fi statistics, validation flags, and registry-derived effective configuration metadata while preserving exclusive no-clobber creation. The complete output is never duplicated in an in-memory DOM.

**Tech Stack:** C++23, ns-3, `nlohmann::json` scalar encoding, CMake, ns-3 `TestSuite`.

**Spec:** `docs/superpowers/specs/2026-08-25-experiment-json-output-design.md`

## Global Constraints

- Backward compatibility with the previous JSON schema is not required.
- Prefer precise names over aliases; do not emit old and new keys together.
- Measurement fields use `_us`, `_ms`, `_s`, `_bytes`, `_mbps`, or `_percent` suffixes where applicable; quantities use `_count`.
- Preserve all information currently emitted by `PrintTransmissionTimePerSender()` and `PrintCrossLayerReport()`, including per-second rows and drop breakdowns.
- End-of-experiment measurement summaries must not be written to stdout or ns-3 logs.
- `experiment_metadata.configuration` is physically last and contains exactly all 36 effective typed values grouped into the eight TOML sections.
- Do not include resolved paths or per-field source/provenance metadata.
- Keep sparse Wi-Fi output streaming and bounded-memory; use `nlohmann::json` only for small scalar/configuration encoding.
- Preserve C++23 exclusive no-clobber output creation and open/write/flush/close error propagation.
- Change the compiled, starter-config, documented, and ignored default filename to `output.json`.
- Keep non-vendored implementation files below 600 lines and document new public APIs with ns-3 Doxygen conventions.
- Update English and Russian documentation together.
- Do not modify files under `traces/` or outer ns-3 source/user files.
- Build and test with examples, tests, logs, warnings, and warnings-as-errors enabled.

---

### Task 1: Effective configuration JSON and new output default

**Files:**
- Modify: `.gitignore`
- Modify: `config/basic_config.toml`
- Modify: `examples/scenario-config.h`
- Modify: `examples/scenario-config-internal.h`
- Modify: `examples/scenario-config.cc`
- Create: `examples/scenario-config-json.cc`
- Modify: `examples/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Create: `test/scenario-config-json-test-suite.cc`
- Modify: `test/scenario-config-test-suite.cc`
- Modify: `test/llm-test-suite.h`
- Modify: `test/llm-test-suite.cc`

**Interfaces:**
- Consumes: the existing 36-entry `ConfigOption` registry and nested `ScenarioConfig`.
- Produces: `ConfigOption::readJson` and `WriteEffectiveConfigurationJson(std::ostream&, const ScenarioConfig&)` for the final serializer.

- [ ] **Step 1: Write failing default and effective-configuration tests**

Change the literal default assertion in `scenario-config-test-suite.cc`:

```cpp
NS_TEST_ASSERT_MSG_EQ(config.general.outputName, "output.json", "Wrong output name");
```

Create `scenario-config-json-test-suite.cc`. Its first case constructs a
`ScenarioConfig`, sets representative final values, writes it through the new
function, parses the stream, and checks section/field cardinality and scalar
types:

```cpp
ScenarioConfig config;
config.general.traceFile = "traces/quoted-\"name\".json";
config.general.runFolder.reset();
config.general.outputName = "custom-output.json";
config.simulation.durationMode = DurationMode::FIXED;
config.simulation.fixedDurationSeconds = 12.5;
config.topology.isolateBssChannels = false;
config.wifi.band = WifiBandConfig::BAND_6_GHZ;
config.wifi.activeProbing = false;
config.logging.sampleScenarioLevel = "debug";

std::ostringstream output;
WriteEffectiveConfigurationJson(output, config);
const auto document = nlohmann::json::parse(output.str());

NS_TEST_ASSERT_MSG_EQ(document.size(), 8, "Wrong configuration section count");
std::size_t fieldCount = 0;
for (const auto& section : document.items())
{
    fieldCount += section.value().size();
}
NS_TEST_ASSERT_MSG_EQ(fieldCount, 36, "Wrong effective configuration field count");
NS_TEST_ASSERT_MSG_EQ(document.at("general").at("run_folder").is_null(),
                      true,
                      "Omitted run folder is not null");
NS_TEST_ASSERT_MSG_EQ(document.at("general").at("trace_file").get<std::string>(),
                      config.general.traceFile,
                      "Trace string was not escaped and restored");
NS_TEST_ASSERT_MSG_EQ(document.at("simulation").at("duration_mode"),
                      "fixed",
                      "Wrong duration enum spelling");
NS_TEST_ASSERT_MSG_EQ(document.at("simulation").at("fixed_duration_seconds"),
                      12.5,
                      "Float lost its JSON type");
NS_TEST_ASSERT_MSG_EQ(document.at("topology").at("isolate_bss_channels"),
                      false,
                      "Boolean lost its JSON type");
NS_TEST_ASSERT_MSG_EQ(document.at("wifi").at("band"), "6GHz", "Wrong band spelling");
```

Add a second case that obtains every `ConfigOption`, invokes `readJson`, and
asserts no callback is empty. Register `CreateScenarioConfigJsonTestCases()`
in the central suite and add the new source to `test_sources`.

- [ ] **Step 2: Build and verify RED**

From the outer ns-3 root:

```bash
./ns3 build llm-test
```

Expected: compilation fails because `WriteEffectiveConfigurationJson` and
`ConfigOption::readJson` do not exist, and the old default assertion fails once
the API declarations are stubbed.

- [ ] **Step 3: Change the default filename and tracked ignore rule**

Apply these exact values:

```cpp
std::string outputName{"output.json"};
```

```toml
# Plain JSON filename created inside the resolved run folder.
output_name = "output.json"
```

```gitignore
/run/
/output.json
```

Remove `/mac-node-stats.json`; do not retain it as a compatibility ignore rule.

- [ ] **Step 4: Add one registry-derived JSON reader per option**

Include `ns3/json.hpp` in the private header and add:

```cpp
std::function<nlohmann::json(const ScenarioConfig&)> readJson; ///< Effective-value reader.
```

First make each factory infer the existing generic-lambda accessor type instead
of erasing it as `ConfigAccessor<T>`. For example:

```cpp
template <typename T, typename Accessor>
ConfigOption MakeIntegerOption(std::string_view tomlPath,
                               Accessor accessor,
                               std::string_view description);

template <typename Accessor>
ConfigOption MakeStringOption(std::string_view tomlPath,
                              Accessor accessor,
                              std::string_view description);
```

Apply the same `Accessor` template parameter to optional string, float,
Boolean, and enum factories. The registry's existing `[](auto& c) -> auto&`
lambdas then return mutable references for setters and const references for
readers without `const_cast` or a second accessor list.

Extend each factory rather than editing 36 entries manually. The callbacks
have these exact behaviors:

```cpp
// String
option.readJson = [accessor](const ScenarioConfig& config) {
    return nlohmann::json(accessor(config));
};

// Optional string
option.readJson = [accessor](const ScenarioConfig& config) {
    const auto& value = accessor(config);
    return value ? nlohmann::json(*value) : nlohmann::json(nullptr);
};

// Integer, floating point, Boolean
option.readJson = [accessor](const ScenarioConfig& config) {
    return nlohmann::json(accessor(config));
};
```

For enum factories, retain one copy of the canonical string/value table for
the reader before moving the setter's copy:

```cpp
auto readerValues = values;
const auto apply = [path = option.tomlPath,
                    accessor,
                    values = std::move(values)](ScenarioConfig& config,
                                                std::string_view text) {
    for (const auto& [name, value] : values)
    {
        if (text == name)
        {
            accessor(config) = value;
            return;
        }
    }
    ThrowExpected(path, ConfigValueType::ENUM);
};
option.readJson = [path = option.tomlPath,
                   accessor,
                   values = std::move(readerValues)](const ScenarioConfig& config) {
    for (const auto& [name, value] : values)
    {
        if (accessor(config) == value)
        {
            return nlohmann::json(name);
        }
    }
    throw ScenarioConfigError("invalid " + path + ": no canonical enum spelling");
};
```

Validation should make the throw unreachable, but the callback must not emit
an invented fallback spelling.

- [ ] **Step 5: Stream the eight-section configuration object**

Declare in `scenario-config-internal.h`:

```cpp
void WriteEffectiveConfigurationJson(std::ostream& output,
                                     const ScenarioConfig& configuration);
```

Implement it in `scenario-config-json.cc`. Iterate the registry in its existing
order, split each dotted path at the single `.`, open a new section when the
section name changes, and emit `nlohmann::json(fieldName).dump()` plus
`option.readJson(configuration).dump()`. Reject a path with no dot or an empty
part using `ScenarioConfigError`. The produced text is one JSON object and
contains no root `configuration` wrapper.

- [ ] **Step 6: Register sources and verify GREEN**

Add `scenario-config-json.cc` to the example and test source lists, then run:

```bash
./ns3 build llm-test llm_sample
./test.py -s llm --no-build
python3 - <<'PY'
from pathlib import Path
import re
s = Path('contrib/llm/config/basic_config.toml').read_text()
assert len(re.findall(r'^\[[a-z]+\]$', s, re.M)) == 8
assert len(re.findall(r'^[a-z][a-z0-9_]*\s*=', s, re.M)) == 35
assert 'output_name = "output.json"' in s
assert '# run_folder = ' in s
PY
```

Expected: both targets build, the `llm` suite passes, and counts remain 8/35
plus the commented optional run folder.

- [ ] **Step 7: Commit Task 1**

```bash
git -C contrib/llm add .gitignore config CMakeLists.txt examples test
git -C contrib/llm commit -m "llm: Add effective configuration JSON"
```

---

### Task 2: Typed transmission summary

**Files:**
- Create: `examples/experiment-output.h`
- Create: `examples/traffic-flow-monitor-internal.h`
- Create: `examples/traffic-flow-summary.cc`
- Modify: `examples/traffic-flow-monitor.h`
- Modify: `examples/traffic-flow-monitor.cc`
- Modify: `examples/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Create: `test/traffic-flow-summary-test-suite.cc`
- Modify: `test/llm-test-suite.h`
- Modify: `test/llm-test-suite.cc`

**Interfaces:**
- Produces:

```cpp
struct TransmissionSenderSummary
{
    std::string senderIpv4;
    uint64_t matchedPacketCount{0};
    uint64_t totalTransmissionDurationUs{0};
    uint64_t transmittedPayloadBytes{0};
    std::optional<double> effectiveThroughputMbps;
};

struct TransmissionSummary
{
    std::vector<TransmissionSenderSummary> senders;
};

TransmissionSummary TrafficFlowMonitor::BuildTransmissionSummary() const;
```

- [ ] **Step 1: Add the output types with complete Doxygen**

Create `experiment-output.h` with include guards, `<cstdint>`, `<optional>`,
`<string>`, and `<vector>`. Add the two exact types above. Document that bytes
are MAC transmit payload samples and may include repeated attempts, while rate
is null without a positive matched duration.

- [ ] **Step 2: Write failing pure aggregation tests**

Move `FlowKey` and `TrafficFlowMonitorState` into the private
`traffic-flow-monitor-internal.h`, preserving their exact map key and state
types. Declare:

```cpp
TransmissionSummary BuildTransmissionSummary(const TrafficFlowMonitorState& state);
```

Create a test fixture with:

```cpp
TrafficFlowMonitorState state;
const TrafficFlowKey matched{"10.1.0.2", 9000, "10.1.0.1", 10000, 1000};
state.transmitTimestampsByFlow[matched] = {100, 500, 900};
state.receiveTimestampsByFlow[matched] = {300, 800, 850};
state.transmittedBytesBySource["10.1.0.2"] = {1000, 1000, 1000};
state.transmittedBytesBySource["10.1.0.3"] = {3000000000ULL, 1000ULL};

const TransmissionSummary summary = BuildTransmissionSummary(state);
```

Assert sender `10.1.0.2` has two positive matches, 500 us total duration,
3000 bytes, and 48 Mbps. Assert sender `10.1.0.3` remains present with
3000001000 bytes, zero matches/duration, and a null rate. This proves both
nonpositive-duration handling and accumulation above `INT_MAX`.

- [ ] **Step 3: Build and verify RED**

Register `CreateTrafficFlowSummaryTestCases()` and add the production monitor,
summary source, and test source to `test_sources`:

```bash
./ns3 build llm-test
```

Expected: fails because the summary types/function do not exist.

- [ ] **Step 4: Implement the pure builder**

Implement in `traffic-flow-summary.cc`:

1. Start a sender accumulator for every entry in
   `transmittedBytesBySource` using `std::accumulate(..., uint64_t{0})`.
2. For each received flow, pair entries through
   `min(tx.size(), rx.size())`.
3. Count and sum only `receiveUs - transmitUs > 0` under `flow.sourceIp`.
4. Emit records in the sender map's deterministic order.
5. Set rate to:

```cpp
static_cast<double>(bytes) * 8.0 / static_cast<double>(durationUs)
```

   when duration is positive; otherwise use `std::nullopt`.

Implement the public member by delegating to the private pure function.

- [ ] **Step 5: Keep transitional logging derived from the typed summary**

Retain `PrintTransmissionTimePerSender()` only until Task 4 keeps the tree
buildable. Change it to call `BuildTransmissionSummary()` and log those records;
remove the old independent aggregation block. This prevents two algorithms
from drifting during migration.

- [ ] **Step 6: Build, test, size-check, and commit**

```bash
./ns3 build llm-test llm_sample
./test.py -s llm --no-build
wc -l contrib/llm/examples/traffic-flow-monitor.cc \
  contrib/llm/examples/traffic-flow-summary.cc \
  contrib/llm/examples/traffic-flow-monitor-internal.h
git -C contrib/llm diff --check
git -C contrib/llm add CMakeLists.txt examples test
git -C contrib/llm commit -m "llm: Build typed transmission summaries"
```

Expected: tests pass and each focused implementation file remains below 600
lines.

---

### Task 3: Typed cross-layer summary

**Files:**
- Modify: `examples/experiment-output.h`
- Modify: `examples/wifi-statistics.h`
- Modify: `examples/wifi-statistics-internal.h`
- Modify: `examples/wifi-statistics-report.cc`
- Create: `test/cross-layer-summary-test-suite.cc`
- Modify: `CMakeLists.txt`
- Modify: `test/llm-test-suite.h`
- Modify: `test/llm-test-suite.cc`

**Interfaces:**
- Produces:

```cpp
CrossLayerSummary WifiStatistics::BuildCrossLayerSummary() const;
CrossLayerSummary BuildCrossLayerSummary(const WifiStatisticsState& statistics);
```

- [ ] **Step 1: Add exact cross-layer output types**

Append these types to `experiment-output.h`, using camelCase C++ members whose
JSON spellings are defined in Task 4:

```cpp
struct DelaySummary
{
    uint64_t sampleCount{0};
    double meanUs{0.0};
    double standardDeviationUs{0.0};
    double minimumUs{0.0};
    double maximumUs{0.0};
};

struct MacDropReasonSummary
{
    int reasonCode{0};
    uint64_t dropCount{0};
};

struct AgentDropSummary
{
    std::string agentKey;
    uint64_t dropEventCount{0};
    uint64_t droppedPayloadBytes{0};
};
```

Define the remaining records exactly:

```cpp
struct CrossLayerIntervalSummary
{
    uint64_t intervalIndex{0};
    double intervalStartS{0.0};
    double intervalDurationS{0.0};
    DelaySummary applicationToPhyDelay;
    double applicationTransmitThroughputMbps{0.0};
    double phyPayloadThroughputMbps{0.0};
    double uniquePhyPayloadThroughputMbps{0.0};
    double channelUtilizationPercent{0.0};
    uint64_t phyRetransmissionCount{0};
    uint64_t macTransmitDropCount{0};
    uint64_t macTransmitDropBytes{0};
    uint64_t macMpduDropCount{0};
    uint64_t macMpduDropBytes{0};
    uint64_t macDataFailureCount{0};
    uint64_t macFinalDataFailureCount{0};
    uint64_t applicationDropEventCount{0};
    uint64_t applicationDropBytes{0};
    std::vector<MacDropReasonSummary> macMpduDropsByReason;
    std::vector<AgentDropSummary> applicationDropsByAgent;
};

struct CrossLayerOverallSummary
{
    double experimentDurationS{0.0};
    DelaySummary applicationToPhyDelay;
    uint64_t applicationTransmittedPayloadBytes{0};
    uint64_t phyPayloadBytes{0};
    uint64_t uniquePhyPayloadBytes{0};
    uint64_t phyMpduBytes{0};
    double averageApplicationTransmitThroughputMbps{0.0};
    double averagePhyPayloadThroughputMbps{0.0};
    double averageChannelUtilizationPercent{0.0};
    uint64_t phyRetransmissionCount{0};
    uint64_t macTransmitDropCount{0};
    uint64_t macTransmitDropBytes{0};
    uint64_t macMpduDropCount{0};
    uint64_t macMpduDropBytes{0};
    uint64_t macDataFailureCount{0};
    uint64_t macFinalDataFailureCount{0};
    uint64_t applicationDropEventCount{0};
    uint64_t applicationDropBytes{0};
    std::vector<MacDropReasonSummary> macMpduDropsByReason;
};

struct CrossLayerNodeSummary
{
    uint32_t nodeId{0};
    std::string nodeLabel;
    std::vector<CrossLayerIntervalSummary> oneSecondIntervals;
    CrossLayerOverallSummary overall;
};

struct CrossLayerSummary
{
    std::vector<CrossLayerNodeSummary> nodes;
};
```

Use `uint64_t` for interval indexes, counts, and bytes; use `double` for rates,
durations, percentages, and delay moments.

- [ ] **Step 2: Write failing builder tests in a new file**

Create a 1500 ms `TrafficCoordinator` and a `WifiStatisticsState` with two
registered nodes. Leave one node empty. For the populated node, fill seconds 0
and 1 with literal `DelayAccumulator` samples, bytes, busy microseconds,
retransmissions, drops, reason codes, and agent drops:

```cpp
WifiStatisticsState state(coordinator, 10);
state.nodeLabels[7] = "AP0";
state.nodeLabels[8] = "STA0";

auto& first = state.nodeSeconds[7][0];
first.appToPhy.Add(100.0);
first.appToPhy.Add(300.0);
first.appTxBytes = 1000000;
first.phyPayloadBytes = 500000;
first.phyUniquePayloadBytes = 400000;
first.phyBusyUs = 250000;
first.phyRetransmissions = 3;
first.macTxDrops = 2;
first.macTxDropBytes = 2000;
first.macMpduDrops = 1;
first.macMpduDropBytes = 1000;
first.macMpduDropsByReason[4] = 1;
first.macDataFailures = 5;
first.macFinalDataFailures = 1;
first.appDropEvents = 2;
first.appDropBytes = 4096;
first.appDropsByAgent["agent-1"] = {2, 4096};

auto& last = state.nodeSeconds[7][1];
last.appToPhy.Add(500.0);
last.appTxBytes = 250000;
last.phyPayloadBytes = 125000;
last.phyUniquePayloadBytes = 100000;
last.phyBusyUs = 125000;
```

Assert:

- two nodes and two intervals per node;
- final interval duration is `0.5` s;
- first delay summary is count 2, mean 200 us, population stddev 100 us,
  min 100 us, max 300 us;
- first app/PHY/unique throughputs are 8/4/3.2 Mbps;
- first channel utilization is 25 percent;
- drop reasons and agent drops retain exact values;
- the overall 1500 ms totals and rates match literal calculations;
- the empty node has zero-valued intervals/overall and empty breakdown arrays.

- [ ] **Step 3: Build and verify RED**

Register `CreateCrossLayerSummaryTestCases()` and run:

```bash
./ns3 build llm-test
```

Expected: fails because the output types and builders are absent.

- [ ] **Step 4: Implement one pure summary algorithm**

Declare the free builder in `wifi-statistics-internal.h`; implement it inside
`wifi-statistics-report.cc` and make the public member delegate to it.

For every node in `nodeLabels`, iterate:

```cpp
uint64_t totalSecondBuckets =
    durationMs > 0 ? static_cast<uint64_t>(ceil(durationMs / 1000.0)) : 0;
intervalDurationS = max(0.0, min(1.0, durationS - intervalIndex));
```

Use zero `NodeSecondStats` for absent buckets. Reuse the existing formulas:

```text
throughput_mbps = bytes * 8 / 1e6 / interval_duration_s
channel_utilization_percent = min(100, busy_us /
                                  (interval_duration_s * 1e6) * 100)
```

Build `DelaySummary` from the accumulator, forcing min/max to zero for no
samples. Merge overall accumulators/totals exactly once per interval. Convert
ordered reason and agent maps to deterministic vectors.

- [ ] **Step 5: Keep the transitional print method derived from typed records**

Until Task 4 updates main, retain `PrintCrossLayerReport()` but make it call
`BuildCrossLayerSummary()` and log the returned records. Delete all direct
access to `WifiStatisticsState` from the formatting portion. The typed builder
is the only measurement algorithm.

- [ ] **Step 6: Build, test, and commit**

```bash
./ns3 build llm-test llm_sample
./test.py -s llm --no-build
./utils/check-style-clang-format.py \
  contrib/llm/examples/experiment-output.h \
  contrib/llm/examples/wifi-statistics-report.cc \
  contrib/llm/test/cross-layer-summary-test-suite.cc
git -C contrib/llm diff --check
git -C contrib/llm add CMakeLists.txt examples test
git -C contrib/llm commit -m "llm: Build typed cross-layer summaries"
```

---

### Task 4: Complete streaming experiment serializer

**Files:**
- Create: `examples/experiment-output-internal.h`
- Create: `examples/experiment-json.cc`
- Create: `examples/transmission-summary-json.cc`
- Create: `examples/cross-layer-summary-json.cc`
- Create: `examples/wifi-statistics-summary.cc`
- Modify: `examples/wifi-statistics-json.cc`
- Modify: `examples/wifi-statistics.h`
- Modify: `examples/wifi-statistics-internal.h`
- Modify: `examples/traffic-flow-monitor.h`
- Modify: `examples/traffic-flow-monitor.cc`
- Delete: `examples/wifi-statistics-report.cc`
- Modify: `examples/sample-scenario.cc`
- Modify: `examples/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Create: `test/experiment-json-test-suite.cc`
- Modify: `test/wifi-statistics-test-suite.cc`
- Modify: `test/llm-test-suite.h`
- Modify: `test/llm-test-suite.cc`

**Interfaces:**
- Consumes: typed summaries from Tasks 2-3 and
  `WriteEffectiveConfigurationJson()` from Task 1.
- Produces the final public writer:

```cpp
void WifiStatistics::WriteExperimentJson(
    const std::string& outputPath,
    const TransmissionSummary& transmissionSummary,
    const CrossLayerSummary& crossLayerSummary,
    const ScenarioConfig& configuration) const;
```

- [ ] **Step 1: Move JSON-specific tests into a focused suite and make them fail on the new schema**

Move the existing sparse JSON, 64-bit window, and output failure cases out of
`wifi-statistics-test-suite.cc` into `experiment-json-test-suite.cc`. This keeps
both test files below 600 lines. Register `CreateExperimentJsonTestCases()`.

Update the real JSON fixture to build literal `TransmissionSummary`,
`CrossLayerSummary`, and a representative `ScenarioConfig`, then call the new
writer. Assert the exact root members:

```cpp
NS_TEST_ASSERT_MSG_EQ(document.at("schema_version"), 1, "Wrong schema version");
NS_TEST_ASSERT_MSG_EQ(document.contains("measurement_semantics"), true, "Missing semantics");
NS_TEST_ASSERT_MSG_EQ(document.at("statistics_window_ms"), 25, "Wrong window width");
NS_TEST_ASSERT_MSG_EQ(document.contains("wifi_windows"), true, "Missing Wi-Fi windows");
NS_TEST_ASSERT_MSG_EQ(document.contains("wifi_summary"), true, "Missing Wi-Fi summary");
NS_TEST_ASSERT_MSG_EQ(document.contains("transmission_summary"), true,
                      "Missing transmission summary");
NS_TEST_ASSERT_MSG_EQ(document.contains("cross_layer_summary"), true,
                      "Missing cross-layer summary");
NS_TEST_ASSERT_MSG_EQ(document.contains("validation"), true, "Missing validation");
NS_TEST_ASSERT_MSG_EQ(document.contains("experiment_metadata"), true, "Missing metadata");
```

Assert literal values for every renamed Wi-Fi flow field, one sender, one
cross-layer interval/overall row, both validation flags, and representative
configuration values. Assert `general.run_folder` is null and the root has no
`resolved_paths` member. Recursively collect every object key and assert none
of these removed keys occurs:

```text
source, byte_semantics, phy_rate_semantics, window_ms, windows, summary,
timestamp, stats, ap_id, up_flows, down_flows, up_total_bytes,
down_total_bytes, host_id, bytes, bw, avg_phy_data_rate_mbps,
phy_tx_attempts, phy_tx_airtime_us, window_totals_consistent,
summary_totals_consistent
```

Read the raw file and assert:

```cpp
raw.find("\"validation\"") < raw.find("\"experiment_metadata\"")
```

Keep failure tests for existing output and missing parent, but call
`WriteExperimentJson()`.

- [ ] **Step 2: Build and verify RED**

```bash
./ns3 build llm-test
```

Expected: fails because `WriteExperimentJson` and the focused internal writers
do not exist.

- [ ] **Step 3: Define focused internal writer contracts**

Create `experiment-output-internal.h` with:

```cpp
struct WifiJsonValidation
{
    bool windowPayloadTotalsConsistent{true};
    bool summaryPayloadTotalsConsistent{true};
};

WifiJsonValidation WriteWifiStatisticsJsonMembers(std::ostream& output,
                                                   const WifiStatisticsState& statistics);
void WriteTransmissionSummaryJson(std::ostream& output,
                                  const TransmissionSummary& summary);
void WriteCrossLayerSummaryJson(std::ostream& output,
                                const CrossLayerSummary& summary);
```

Add one small inline helper that writes a scalar through
`nlohmann::json(value).dump()`; use it for every string and nullable scalar:

```cpp
template <typename T>
void WriteJsonScalar(std::ostream& output, const T& value)
{
    output << nlohmann::json(value).dump();
}
```

- [ ] **Step 4: Refactor Wi-Fi serialization to the precise breaking schema**

Move file opening/closing out of `wifi-statistics-json.cc`. Implement
`WriteWifiStatisticsJsonMembers()` to write, in order:

```text
"statistics_window_ms"
"wifi_windows"
"wifi_summary"
```

Rename every field exactly as specified. Use nested `uplink`/`downlink`
objects with `total_payload_bytes` and `flows`. Return both validation flags
instead of writing `validation` directly. Retain sparse-window range checks,
summary-vs-sparse checks, 64-bit IDs/counts/timestamps, and the exact rate
formula.

Apply this exact old-to-new mapping in window output:

```text
timestamp                    -> window_end_ms
stats                        -> access_points
ap_id                        -> access_point_id
up_flows                     -> uplink.flows
up_total_bytes               -> uplink.total_payload_bytes
down_flows                   -> downlink.flows
down_total_bytes             -> downlink.total_payload_bytes
host_id                      -> station_ipv4
bytes                        -> payload_bytes
bw                           -> throughput_mbps
avg_phy_data_rate_mbps       -> average_phy_data_rate_mbps
phy_tx_attempts              -> phy_transmission_attempt_count
phy_tx_airtime_us            -> phy_transmission_airtime_us
```

The summary uses the same direction objects. Its flow byte field is
`total_payload_bytes` instead of the per-window `payload_bytes`.

- [ ] **Step 5: Implement the typed-summary JSON writers**

`transmission-summary-json.cc` writes:

```json
{"senders": [{
  "sender_ipv4": "...",
  "matched_packet_count": 0,
  "total_transmission_duration_us": 0,
  "transmitted_payload_bytes": 0,
  "effective_throughput_mbps": null
}]}
```

`cross-layer-summary-json.cc` writes every exact interval/overall field and
drop array. Use these exact member-to-key mappings:

```text
nodeId                                      -> node_id
nodeLabel                                   -> node_label
oneSecondIntervals                          -> one_second_intervals
intervalIndex                               -> interval_index
intervalStartS                              -> interval_start_s
intervalDurationS                           -> interval_duration_s
applicationToPhyDelay                       -> application_to_phy_delay
applicationTransmitThroughputMbps           -> application_transmit_throughput_mbps
phyPayloadThroughputMbps                    -> phy_payload_throughput_mbps
uniquePhyPayloadThroughputMbps              -> unique_phy_payload_throughput_mbps
channelUtilizationPercent                   -> channel_utilization_percent
phyRetransmissionCount                      -> phy_retransmission_count
macTransmitDropCount                        -> mac_transmit_drop_count
macTransmitDropBytes                        -> mac_transmit_drop_bytes
macMpduDropCount                            -> mac_mpdu_drop_count
macMpduDropBytes                            -> mac_mpdu_drop_bytes
macDataFailureCount                         -> mac_data_failure_count
macFinalDataFailureCount                    -> mac_final_data_failure_count
applicationDropEventCount                   -> application_drop_event_count
applicationDropBytes                        -> application_drop_bytes
macMpduDropsByReason                        -> mac_mpdu_drops_by_reason
applicationDropsByAgent                     -> application_drops_by_agent
experimentDurationS                         -> experiment_duration_s
applicationTransmittedPayloadBytes          -> application_transmitted_payload_bytes
phyPayloadBytes                             -> phy_payload_bytes
uniquePhyPayloadBytes                       -> unique_phy_payload_bytes
phyMpduBytes                                -> phy_mpdu_bytes
averageApplicationTransmitThroughputMbps    -> average_application_transmit_throughput_mbps
averagePhyPayloadThroughputMbps             -> average_phy_payload_throughput_mbps
averageChannelUtilizationPercent            -> average_channel_utilization_percent
```

Delay fields are `sample_count`, `mean_us`, `standard_deviation_us`,
`minimum_us`, and `maximum_us`. Reason entries are `reason_code` and
`drop_count`. Agent entries are `agent_key`, `drop_event_count`, and
`dropped_payload_bytes`. Use scalar encoding for `node_label` and `agent_key`.
Never derive measurements in these files; they serialize typed values only.

- [ ] **Step 6: Implement the root writer and preserve I/O safety**

In `experiment-json.cc`, implement the public member. Open with:

```cpp
std::ofstream output(outputPath, std::ios::out | std::ios::noreplace);
```

Throw with the output path if exclusive creation fails. Stream root members in
the exact spec order. `measurement_semantics` uses the exact keys
`mac_payload_source`, `mac_payload_byte_semantics`, and
`phy_data_rate_semantics`; retain the current three semantic descriptions.
Write `validation` from `WifiJsonValidation` under the keys
`window_payload_totals_consistent` and
`summary_payload_totals_consistent`, then write:

```cpp
output << ",\n  \"experiment_metadata\": {\n"
       << "    \"configuration\": ";
WriteEffectiveConfigurationJson(output, configuration);
output << "\n  }\n}\n";
```

Check stream state after body write, flush, and close using the existing error
wording pattern, updated from "PHY statistics output" to "experiment output".

- [ ] **Step 7: Integrate main and remove report-output APIs**

In `sample-scenario.cc`, build both summaries before opening the output and
write them inside the existing output error boundary:

```cpp
try
{
    const TransmissionSummary transmissionSummary =
        trafficFlowMonitor.BuildTransmissionSummary();
    const CrossLayerSummary crossLayerSummary = wifiStatistics.BuildCrossLayerSummary();
    wifiStatistics.WriteExperimentJson(resolvedPaths.outputFile.string(),
                                       transmissionSummary,
                                       crossLayerSummary,
                                       config);
}
catch (const std::exception& error)
{
    Simulator::Destroy();
    std::cerr << "error: cannot write experiment output '"
              << resolvedPaths.outputFile.string() << "': " << error.what() << std::endl;
    return 1;
}
```

Delete both calls to the old print methods. Remove their declarations and
implementations. Rename `wifi-statistics-report.cc` to
`wifi-statistics-summary.cc` in both CMake lists. Remove `WriteJson()` and the
old complete-root free writer after all callers use `WriteExperimentJson()`.

- [ ] **Step 8: Build, test, format, and commit**

```bash
./utils/check-style-clang-format.py --fix \
  contrib/llm/examples contrib/llm/test
./utils/check-style-clang-format.py \
  contrib/llm/examples contrib/llm/test
./ns3 build llm-test llm_sample
./test.py -s llm --no-build
git -C contrib/llm diff --check
wc -l contrib/llm/examples/*.cc contrib/llm/examples/*.h | sort -nr | head -20
git -C contrib/llm add CMakeLists.txt examples test
git -C contrib/llm commit -m "llm: Write complete experiment JSON"
```

Expected: schema and summary tests pass, all old report APIs/keys are absent,
and every implementation file remains below 600 lines.

---

### Task 5: Documentation, public behavior, and final verification

**Files:**
- Modify: `README.md`
- Modify: `README_RU.md`
- Modify: `test/examples-to-run.py` only if its explicit output name or checks need wording alignment
- Modify: `config/basic_config.toml` only for final comment precision

**Interfaces:**
- Consumes: the final schema and APIs from Tasks 1-4.
- Produces: documented breaking JSON contract and verified public experiment output.

- [ ] **Step 1: Update English output documentation completely**

Replace the old `source/window_ms/windows/summary` tables and the "Log-only
cross-layer metrics" section with the exact root/member schema from the spec.
Document:

- `schema_version = 1` and intentional breaking change;
- every renamed Wi-Fi window/summary field and unit;
- transmission matching semantics and SI Mbps formula;
- every cross-layer interval/overall field and drop breakdown;
- both validation flags;
- all 36 effective configuration values under the final metadata member;
- configured rather than resolved paths in metadata;
- default `output.json` and no-clobber behavior;
- summary measurements are not written to stdout/logs.

Remove stale claims that cross-layer metrics, `cwnd`, or agent/station share are
printed by `PrintCrossLayerReport()` when they are not part of the new schema.

- [ ] **Step 2: Mirror the Russian documentation**

Translate explanations naturally while keeping schema keys, formulas,
commands, example JSON, field tables, URLs, and links identical. Update every
default-name mention from `mac-node-stats.json` to `output.json` in both files.

- [ ] **Step 3: Verify a public short experiment and absence of report logs**

Use a validated temporary directory:

```bash
run_tmp="$(mktemp -d /tmp/llm-output-smoke.XXXXXX)"
NS_LOG="SampleScenario=level_warn" ./ns3 run \
  "llm_sample --config contrib/llm/config/basic_config.toml \
  --general-trace-file contrib/llm/test/data/minimal-trace.json \
  --general-run-folder $run_tmp \
  --simulation-duration-mode fixed \
  --simulation-fixed-duration-seconds 0.2" \
  >"$run_tmp/console.txt" 2>&1
test -s "$run_tmp/output.json"
python3 - "$run_tmp/output.json" <<'PY'
import json, sys
with open(sys.argv[1], encoding='utf-8') as stream:
    document = json.load(stream)
assert document['schema_version'] == 1
configuration = document['experiment_metadata']['configuration']
assert len(configuration) == 8
assert sum(len(section) for section in configuration.values()) == 36
assert configuration['general']['output_name'] == 'output.json'
assert 'transmission_summary' in document
assert 'cross_layer_summary' in document
PY
! rg -q 'MAC Layer Transmission time per sender|App -> PHY / reliability statistics|\[Node stats\]|\[Node overall\]|\[MAC MPDU drop overall\]' "$run_tmp/console.txt"
RUN_TMP="$run_tmp" python3 -c \
  'import os,shutil; p=os.environ["RUN_TMP"]; assert p.startswith("/tmp/llm-output-smoke."); shutil.rmtree(p)'
```

Expected: the default file exists and parses, metadata is 8/36, and no removed
summary row/banner is present in captured output.

- [ ] **Step 4: Run bilingual schema/default parity scans**

Verify that all required schema keys occur in both documents and stale keys do
not occur in their output-schema sections. Compare code-formatted schema keys
and formulas after excluding localized anchors/language selector links. Assert
both documents contain `output.json` and neither contains
`mac-node-stats.json`.

- [ ] **Step 5: Run the clean full verification gate**

```bash
./ns3 clean
./ns3 configure --enable-examples --enable-tests --enable-logs \
  --enable-warnings --enable-werror
./utils/check-style-clang-format.py \
  contrib/llm/examples contrib/llm/model contrib/llm/test
./ns3 build llm-test llm_sample
./test.py -s llm
./test.py -e 'llm_sample*'
git -C contrib/llm diff --check
```

Then check structure and cleanliness:

```bash
wc -l contrib/llm/examples/*.cc contrib/llm/examples/*.h | sort -nr | head -25
git -C contrib/llm status --short
test ! -d run
test ! -d contrib/llm/run
test ! -e output.json
test ! -e contrib/llm/output.json
test -z "$(find /tmp -maxdepth 1 -type d -name 'llm-output-smoke.*' -print -quit)"
```

Expected: all tests pass; no implementation file exceeds 600 lines; no run,
output, temporary, trace, or outer source artifact remains.

- [ ] **Step 6: Commit final migration**

```bash
git -C contrib/llm add README.md README_RU.md config test/examples-to-run.py
git -C contrib/llm commit -m "llm: Document experiment JSON output"
```

- [ ] **Step 7: Review the complete implementation range**

```bash
git -C contrib/llm log --oneline 8d4b9d1..HEAD
git -C contrib/llm diff --stat 8d4b9d1..HEAD
git -C contrib/llm diff --name-only 8d4b9d1..HEAD | rg '^traces/' && exit 1 || true
```

Expected: five focused implementation commits, no trace-data changes, and no
outer ns-3 changes.
