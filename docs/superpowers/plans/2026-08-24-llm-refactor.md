# LLM Module Refactor Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Refactor the ns-3 `llm` contrib module into focused, behavior-compatible, well-tested components with explicit state ownership and clear naming.

**Architecture:** Preserve every existing public API through compatibility headers while extracting pure parsing, distribution, scheduling, and aggregation logic. Keep AP and STA generators separate, replace scenario globals with a few state-owning objects, and reduce the example entry point to configuration and orchestration.

**Tech Stack:** C++23, ns-3 TypeId/Callback/TestSuite APIs, CMake `build_lib` and `build_lib_example`, nlohmann JSON, Ninja, clang-format 20-22.

**Spec:** `docs/superpowers/specs/2026-08-24-llm-refactor-design.md`

## Global Constraints

- Preserve the `llm_sample` positional CLI, messages, exit behavior, logs, trace sources, JSON schema, timing, callback order, random seed, topology, and address assignment.
- Preserve all existing public class names, TypeId names, method signatures, public structures, and exported header names.
- Keep `APGenerator` and `StaLlmGenerator` as separate classes; do not introduce a common base class.
- Do not introduce strategy interfaces, dependency injection, an event bus, or a generic statistics framework.
- Use ASCII in comments and Doxygen documentation.
- Use `@param`, `@return`, and `@see` Doxygen tags and document all new public members.
- Prefer names with units such as `traceTimeMs`, `timestampUs`, and `payloadBytes`.
- Build and test from `/home/bsa/projects/ns-3-dev`; perform Git operations against `contrib/llm` only.
- Configure with examples, tests, logs, warnings, and warnings-as-errors enabled.
- Every task must keep all previously completed tests green.

---

## Locked file map

### Public model files

- `model/agent-data.h`: existing trace and distribution result structures.
- `model/app-tx-tag.{h,cc}`: `AppTxTag` and byte-tag attachment helper.
- `model/trace-parser.{h,cc}`: stream parser and `ParseJsonFile()`.
- `model/agent-distribution.{h,cc}`: compatibility facade and simple distributor.
- `model/contention-aware-agent-distribution.{h,cc}`: public contention-aware API and coordinator.
- `model/traffic-schedule.{h,cc}`: pure legacy-input conversion, sorting, and time helpers.
- Existing generator and sink files retain their public classes.

### Private model file

- `model/contention-aware-distribution-internal.h`: private activity records and phase declarations.
- `model/contention-aware-bss-assignment.cc`: BSS phase.
- `model/contention-aware-sta-assignment.cc`: STA phase.

### Example files

- `examples/sample-scenario.cc`: process entry point only.
- `examples/scenario-config.{h,cc}`: positional CLI parsing and validation.
- `examples/scenario-topology.{h,cc}`: AP/STA construction and application installation.
- `examples/traffic-coordinator.{h,cc}`: readiness barrier, epoch, and application lifetimes.
- `examples/traffic-flow-monitor.{h,cc}`: device TX/RX matching and report.
- `examples/wifi-statistics.{h,cc}`: Wi-Fi callbacks and aggregation.
- `examples/wifi-statistics-json.cc`: current JSON serialization.

### Tests

- `test/llm-test-suite.{h,cc}`: one registered suite named `llm` and test-case factories.
- `test/trace-parser-test-suite.cc`: parser characterization.
- `test/agent-distribution-test-suite.cc`: both public distributors.
- `test/app-tx-tag-test-suite.cc`: byte-tag round trip.
- `test/traffic-schedule-test-suite.cc`: scheduling helpers.
- `test/traffic-coordinator-test-suite.cc`: epoch and readiness calculations.
- `test/wifi-statistics-test-suite.cc`: window and aggregation behavior.
- `test/scenario-config-test-suite.cc`: CLI parsing behavior.
- `test/data/minimal-trace.json`: deterministic fixture and smoke input.
- `test/examples-to-run.py`: registered lightweight smoke invocation.

---

### Task 1: Add baseline characterization tests

**Files:**
- Create: `test/data/minimal-trace.json`
- Create: `test/llm-test-suite.h`
- Create: `test/llm-test-suite.cc`
- Create: `test/trace-parser-test-suite.cc`
- Create: `test/agent-distribution-test-suite.cc`
- Create: `test/app-tx-tag-test-suite.cc`
- Modify: `CMakeLists.txt:1-32`

**Interfaces:**
- Consumes: current `ParseJsonFile()`, `DistributeAgents()`, `DistributeAgentsContentionAware()`, and `AppTxTag` APIs.
- Produces: one ns-3 suite named `llm`, a shared fixture, and test factories used by later tasks.

- [ ] **Step 1: Add the deterministic trace fixture**

Create `test/data/minimal-trace.json` with exactly:

```json
{
  "traces": [
    {
      "agentId": 1,
      "agentType": "planner",
      "tasks": [
        {
          "operations": [
            {
              "startOffsetMs": 0.0,
              "durationMs": 10.0,
              "downlinkBytes": 100,
              "uplinkBytes": 50
            },
            {
              "startOffsetMs": 100.0,
              "durationMs": 25.0,
              "downlinkBytes": 0,
              "uplinkBytes": 0
            }
          ]
        }
      ]
    },
    {
      "agentId": 2,
      "agentType": "worker",
      "tasks": [
        {
          "operations": [
            {
              "startOffsetMs": 0.0,
              "durationMs": 10.0,
              "downlinkBytes": 200,
              "uplinkBytes": 80
            }
          ]
        }
      ]
    }
  ]
}
```

- [ ] **Step 2: Add the central suite and factory interface**

Create `test/llm-test-suite.h`:

```cpp
#ifndef LLM_TEST_SUITE_H
#define LLM_TEST_SUITE_H

#include "ns3/test.h"

#include <vector>

std::vector<ns3::TestCase*> CreateTraceParserTestCases();
std::vector<ns3::TestCase*> CreateAgentDistributionTestCases();
std::vector<ns3::TestCase*> CreateAppTxTagTestCases();
std::vector<ns3::TestCase*> CreateTrafficScheduleTestCases();
std::vector<ns3::TestCase*> CreateTrafficCoordinatorTestCases();
std::vector<ns3::TestCase*> CreateWifiStatisticsTestCases();
std::vector<ns3::TestCase*> CreateScenarioConfigTestCases();

#endif
```

Create `test/llm-test-suite.cc` with a `TestSuite("llm", TestSuite::Type::UNIT)`.
Its constructor must add every pointer returned by each non-empty factory with
`AddTestCase(testCase, TestCase::Duration::QUICK)`. Define the later-task
factories in this file to return empty vectors until their test files replace
those definitions.

- [ ] **Step 3: Add parser characterization cases**

In `test/trace-parser-test-suite.cc`, create cases that call:

```cpp
const auto tracePath =
    std::string(NS_TEST_SOURCEDIR) + "/data/minimal-trace.json";
const ParsedResult parsed = ParseJsonFile(tracePath);
```

Assert exactly:

```cpp
NS_TEST_ASSERT_MSG_EQ(parsed.agents.size(), 2, "Unexpected agent count");
NS_TEST_ASSERT_MSG_EQ_TOL(parsed.experimentDurationMs,
                          125.0,
                          1e-9,
                          "Filtered local operation must extend duration");
NS_TEST_ASSERT_MSG_EQ(parsed.agents[0].key, "1_planner", "Unexpected first key");
NS_TEST_ASSERT_MSG_EQ(parsed.agents[0].type, 1, "Unexpected planner type");
NS_TEST_ASSERT_MSG_EQ(parsed.agents[0].operations.size(),
                      1,
                      "Zero-byte operation must be filtered");
NS_TEST_ASSERT_MSG_EQ(parsed.agents[1].key, "2_worker", "Unexpected second key");
NS_TEST_ASSERT_MSG_EQ(parsed.agents[1].type, 2, "Unexpected worker type");
```

Return the cases from `CreateTraceParserTestCases()`.

- [ ] **Step 4: Add exact distribution characterization cases**

Construct this input directly in `test/agent-distribution-test-suite.cc`:

```cpp
ParsedResult parsed;
parsed.agents = {
    {"1_planner", 1, 1, {{100, 0.0, 10.0, 50}}},
    {"2_worker", 2, 2, {{200, 0.0, 10.0, 80}}},
};
```

For `DistributeAgents(parsed, 2, 2, 1)`, assert that `2_worker` maps to
`10.1.0.2:9000` and `1_planner` maps to `10.1.1.2:9000`.

For this configuration:

```cpp
ContentionAwareDistributionConfig config;
config.nAp = 2;
config.nStationsPerAp = 2;
config.maxAgentsPerStation = 1;
config.lowContentionPriority = true;
config.slotMs = 50;
```

assert that `1_planner` maps to `10.1.0.2:9000` and `2_worker` maps to
`10.1.1.2:9000`. Call the contention-aware function twice and assert identical
maps. Add cases asserting `std::invalid_argument` for zero APs, zero stations,
negative agent capacity, zero slot width, and insufficient capacity.

- [ ] **Step 5: Add the packet-tag round-trip case**

In `test/app-tx-tag-test-suite.cc`, attach this tag to a 64-byte packet:

```cpp
AppTxTag original(42,
                  123456,
                  Ipv4Address("10.1.0.1"),
                  Ipv4Address("10.1.0.2"),
                  10000,
                  9000,
                  64,
                  "1_planner");
Ptr<Packet> packet = Create<Packet>(64);
packet->AddByteTag(original);
AppTxTag restored;
const bool found = packet->FindFirstMatchingByteTag(restored);
```

Assert `found` and every getter value. Return the case from
`CreateAppTxTagTestCases()`.

- [ ] **Step 6: Register the test sources**

Add this list before `build_lib()` and pass it through `TEST_SOURCES`:

```cmake
set(test_sources
    test/llm-test-suite.cc
    test/trace-parser-test-suite.cc
    test/agent-distribution-test-suite.cc
    test/app-tx-tag-test-suite.cc
)
```

- [ ] **Step 7: Run the baseline suite**

Run:

```bash
./ns3 configure --enable-examples --enable-tests --enable-logs --enable-warnings --enable-werror
./ns3 build llm-test llm_sample
./test.py -s llm
```

Expected: configuration succeeds, both targets build, and the `llm` suite
passes. These are characterization tests, so they must pass before production
files move.

- [ ] **Step 8: Commit the characterization boundary**

```bash
git -C contrib/llm add CMakeLists.txt test docs/superpowers/plans/2026-08-24-llm-refactor.md
git -C contrib/llm commit -m "llm: Add characterization tests"
```

---

### Task 2: Extract agent data, packet tag, and trace parser

**Files:**
- Create: `model/agent-data.h`
- Create: `model/app-tx-tag.h`
- Create: `model/app-tx-tag.cc`
- Create: `model/trace-parser.h`
- Create: `model/trace-parser.cc`
- Modify: `model/agent-distribution.h:1-246`
- Modify: `model/agent-distribution.cc:1-365`
- Modify: `test/trace-parser-test-suite.cc`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: existing public structures and `AppTxTag` methods.
- Produces: `ParsedResult ParseJson(std::istream& input)` while preserving
  `ParsedResult ParseJsonFile(const std::string& jsonPath)`.

- [ ] **Step 1: Add a failing stream-parser test**

Add a case that includes `ns3/trace-parser.h`, constructs this input, and calls
`ParseJson(input)`:

```cpp
std::istringstream input(R"({
  "traces": [{
    "agentId": 7,
    "agentType": "worker",
    "tasks": [{"operations": [{
      "startOffsetMs": 5.0,
      "durationMs": 15.0,
      "downlinkBytes": 8,
      "uplinkBytes": 9
    }]}]
  }]
})");
const ParsedResult parsed = ParseJson(input);
NS_TEST_ASSERT_MSG_EQ(parsed.agents[0].key, "7_worker", "Unexpected key");
```

- [ ] **Step 2: Run the test to verify the missing API**

Run:

```bash
./ns3 build llm-test
```

Expected: FAIL because `ns3/trace-parser.h` or `ParseJson` does not exist.

- [ ] **Step 3: Move public data without changing fields**

Move `Operation`, `AgentInfo`, `ParsedResult`, and `DistributionResult` verbatim
to `model/agent-data.h`. Include that header from `agent-distribution.h` so an
existing consumer that includes only `ns3/agent-distribution.h` still sees all
four types.

- [ ] **Step 4: Move `AppTxTag` declarations and definitions**

Declare the unchanged class in `model/app-tx-tag.h` and move all method bodies
to `model/app-tx-tag.cc`. Keep:

```cpp
TypeId("ns3::AppTxTag").SetParent<Tag>().AddConstructor<AppTxTag>()
```

Add this focused helper without changing tag contents:

```cpp
void AddAppTxTag(Ptr<Packet> packet,
                 Time txTime,
                 const InetSocketAddress& source,
                 const InetSocketAddress& destination,
                 const std::string& agentKey);
```

It constructs `AppTxTag` from the packet UID, timestamp, addresses, ports,
packet size, and agent key, then calls `packet->AddByteTag(tag)`.

- [ ] **Step 5: Extract the stream parser**

Declare in `model/trace-parser.h`:

```cpp
ParsedResult ParseJson(std::istream& input);
ParsedResult ParseJsonFile(const std::string& jsonPath);
```

Move the JSON traversal from `ParseJsonFile()` into `ParseJson()`. Keep the
file wrapper exactly:

```cpp
std::ifstream input(jsonPath);
if (!input)
{
    std::cerr << "Cannot open file: " << jsonPath << std::endl;
    std::exit(1);
}
return ParseJson(input);
```

Do not alter type assignment, operation filtering, duration calculation, or
log text.

- [ ] **Step 6: Update compatibility includes and CMake**

`agent-distribution.h` must include `agent-data.h`, `app-tx-tag.h`, and
`trace-parser.h`. Add the new `.cc` files to `SOURCE_FILES` and all three new
headers to `HEADER_FILES`.

- [ ] **Step 7: Run focused and complete module checks**

```bash
./ns3 build llm-test llm_sample
./test.py -s llm
```

Expected: PASS, including the new stream parser and existing tag tests.

- [ ] **Step 8: Commit the extraction**

```bash
git -C contrib/llm add CMakeLists.txt model test/trace-parser-test-suite.cc
git -C contrib/llm commit -m "llm: Split trace data and parsing"
```

---

### Task 3: Split distribution phases

**Files:**
- Create: `model/contention-aware-distribution-internal.h`
- Create: `model/contention-aware-bss-assignment.cc`
- Create: `model/contention-aware-sta-assignment.cc`
- Modify: `model/agent-distribution.cc`
- Modify: `model/contention-aware-agent-distribution.cc`
- Modify: `model/agent-distribution.h`
- Modify: `model/contention-aware-agent-distribution.h`
- Modify: `test/agent-distribution-test-suite.cc`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `AgentInfo`, `ParsedResult`, `DistributionResult`, and
  `ContentionAwareDistributionConfig`.
- Produces private functions under `ns3::llm_detail`:

```cpp
struct AgentActivity
{
    const AgentInfo* agent{nullptr};
    std::set<int> uplinkSlots;
    int64_t totalBytes{0};
};

int64_t CalculateTotalBytes(const std::vector<Operation>& operations);
std::vector<AgentActivity> BuildAgentActivities(
    const std::vector<AgentInfo>& agents,
    int slotMs);
std::vector<int> AssignAgentsToBss(
    const std::vector<AgentActivity>& activities,
    const ContentionAwareDistributionConfig& config);
void AssignAgentsToStations(
    const std::vector<AgentActivity>& activities,
    const std::vector<int>& bssAssignment,
    int bssIndex,
    const ContentionAwareDistributionConfig& config,
    std::map<std::string, Address>& stationAddressByAgent);
```

- [ ] **Step 1: Extend public characterization around slot boundaries**

Add agents at `startOffsetMs` values `49.999`, `50.0`, and `100.0` with
`slotMs = 50`. Assert that repeated calls produce identical public mappings and
that all three agents are assigned exactly once. Add a zero-agent case and
assert correctly sized empty result vectors.

- [ ] **Step 2: Run characterization before moving functions**

```bash
./ns3 build llm-test
./test.py -s llm
```

Expected: PASS against the original single-file implementation.

- [ ] **Step 3: Create the private activity model**

Create `contention-aware-distribution-internal.h` with the interfaces above.
Move `CalculateTotalBytes()` and `BuildAgentActivities()` into
`contention-aware-agent-distribution.cc` under `ns3::llm_detail`.

- [ ] **Step 4: Move the BSS phase unchanged**

Move the complete current `AssignAgentsToBss()` body and its BSS diagnostics to
`contention-aware-bss-assignment.cc`. Rename only private locals according to
the naming rules. Do not change cost comparisons or tie-break ordering.

- [ ] **Step 5: Move the STA phase unchanged**

Move the complete current `AssignAgentsToStations()` body and its diagnostics
to `contention-aware-sta-assignment.cc`. Preserve both placement-policy branches,
capacity handling, pairwise-affinity calculations, and deterministic ties.

- [ ] **Step 6: Reduce the public coordinator file**

Leave configuration validation, address/result initialization, phase calls,
and completeness checks in `contention-aware-agent-distribution.cc`. Remove
decorative banners and retain comments only for algorithm invariants and
tie-break reasons.

- [ ] **Step 7: Simplify the simple distributor internally**

In `agent-distribution.cc`, use `CalculateTotalBytes()` where behavior matches,
split AP choice and station assignment into small private functions, and retain
the exact public output. Remove the stale multi-paragraph proposal from the
public header while preserving useful API Doxygen.

- [ ] **Step 8: Register sources and run regression tests**

Add both phase `.cc` files to `SOURCE_FILES` and the internal header to
`PRIVATE_HEADER_FILES`, then run:

```bash
./ns3 build llm-test llm_sample
./test.py -s llm
```

Expected: PASS with the exact Task 1 mappings unchanged.

- [ ] **Step 9: Commit the phase split**

```bash
git -C contrib/llm add CMakeLists.txt model test/agent-distribution-test-suite.cc
git -C contrib/llm commit -m "llm: Split distribution phases"
```

---

### Task 4: Extract shared traffic scheduling

**Files:**
- Create: `model/traffic-schedule.h`
- Create: `model/traffic-schedule.cc`
- Create: `test/traffic-schedule-test-suite.cc`
- Modify: `test/llm-test-suite.cc`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: the existing tuple order `(downlinkBytes, endMs, startOffsetMs, uplinkBytes)`.
- Produces:

```cpp
using LegacyAgentOperations =
    std::map<std::string,
             std::vector<std::tuple<int, double, double, int>>>;

struct ScheduledPayload
{
    std::string agentKey;
    uint32_t payloadBytes{0};
    double traceTimeMs{0.0};
};

using DownlinkSchedulesByStation =
    std::map<Address, std::vector<ScheduledPayload>>;

std::vector<ScheduledPayload> BuildUplinkSchedule(
    const LegacyAgentOperations& operationsByAgent);
DownlinkSchedulesByStation BuildDownlinkSchedules(
    const LegacyAgentOperations& operationsByAgent,
    const std::map<std::string, Address>& stationAddressByAgent);
Time GetScheduledSimulationTime(uint64_t experimentStartMs,
                                double traceTimeMs);
uint32_t GetAbsoluteSecond(Time simulationTime);
```

- [ ] **Step 1: Add failing schedule tests**

Create a legacy input with agents `a` and `b`, deliberately inserted out of
time order. Assert:

```cpp
const auto uplink = BuildUplinkSchedule(input);
NS_TEST_ASSERT_MSG_EQ(uplink.size(), 3, "Unexpected uplink count");
NS_TEST_ASSERT_MSG_EQ(uplink[0].traceTimeMs, 10.0, "Wrong first UL time");
NS_TEST_ASSERT_MSG_EQ(uplink[0].payloadBytes, 11, "Wrong first UL bytes");

const Time scheduled = GetScheduledSimulationTime(2000, 12.5);
NS_TEST_ASSERT_MSG_EQ_TOL(scheduled.GetMilliSeconds(),
                          2012.0,
                          1.0,
                          "Unexpected integer millisecond conversion");
NS_TEST_ASSERT_MSG_EQ(GetAbsoluteSecond(Seconds(3.999)),
                      3,
                      "Wrong absolute second");
```

For downlink schedules, assert missing agent addresses are skipped, payloads
use downlink bytes, timestamps use `endMs`, and each station vector is sorted
by `traceTimeMs`.

- [ ] **Step 2: Verify the new API is absent**

```bash
./ns3 build llm-test
```

Expected: FAIL because `traffic-schedule.h` and its functions do not exist.

- [ ] **Step 3: Implement the minimal pure helpers**

Implement tuple conversion with structured bindings. Use `std::sort` with only
`traceTimeMs` as the comparator to match the current AP and STA ordering. Use:

```cpp
return Time::FromDouble(static_cast<double>(experimentStartMs) + traceTimeMs,
                        Time::MS);
```

for schedule conversion and `floor(simulationTime.GetSeconds())` for the
absolute second.

- [ ] **Step 4: Register source, header, and test**

Add `traffic-schedule.cc` to `SOURCE_FILES`, `traffic-schedule.h` to
`HEADER_FILES`, and the test file to `TEST_SOURCES`. Remove the temporary empty
`CreateTrafficScheduleTestCases()` definition from `llm-test-suite.cc`.

- [ ] **Step 5: Run focused tests**

```bash
./ns3 build llm-test
./test.py -s llm
```

Expected: PASS.

- [ ] **Step 6: Commit the scheduling API**

```bash
git -C contrib/llm add CMakeLists.txt model/traffic-schedule.* test
git -C contrib/llm commit -m "llm: Extract traffic scheduling"
```

---

### Task 5: Simplify AP and STA generators

**Files:**
- Modify: `model/ap-generator.h`
- Modify: `model/ap-generator.cc`
- Modify: `model/sta-llm-generator.h`
- Modify: `model/sta-llm-generator.cc`
- Modify: `model/app-tx-tag.h`
- Modify: `model/app-tx-tag.cc`

**Interfaces:**
- Consumes: `LegacyAgentOperations`, `ScheduledPayload`,
  `BuildUplinkSchedule()`, `BuildDownlinkSchedules()`,
  `GetScheduledSimulationTime()`, `GetAbsoluteSecond()`, and `AddAppTxTag()`.
- Produces: unchanged generator public APIs and TypeId trace sources.

- [ ] **Step 1: Capture TypeId compatibility in tests**

Add assertions to the tag or scheduling test file:

```cpp
NS_TEST_ASSERT_MSG_EQ(APGenerator::GetTypeId().GetName(),
                      "ns3::APGenerator",
                      "AP TypeId changed");
NS_TEST_ASSERT_MSG_EQ(StaLlmGenerator::GetTypeId().GetName(),
                      "ns3::StaLlmGenerator",
                      "STA TypeId changed");
```

Also assert that lookups for `Tx`, `AgentSend`, and `AppTxDrop` succeed on the
AP TypeId and `TxCustom`, `AgentSend`, and `AppTxDrop` succeed on the STA
TypeId.

- [ ] **Step 2: Run compatibility tests before edits**

```bash
./ns3 build llm-test
./test.py -s llm
```

Expected: PASS.

- [ ] **Step 3: Replace generator-local tuple conversion**

Keep both public `SetAgentMap()` signatures. Store the input as
`LegacyAgentOperations`, then call the shared builders. Replace
`StationOperation` and the five-element STA tuple with `ScheduledPayload`.

- [ ] **Step 4: Replace duplicated time and tag construction**

Use `GetScheduledSimulationTime()` for AP and STA events,
`GetAbsoluteSecond()` for their current absolute buckets, and `AddAppTxTag()`
for application byte tags. Preserve the current abort conditions and trace
arguments.

- [ ] **Step 5: Improve private names without public changes**

Apply these mappings consistently:

```text
m_agentsMap              -> m_operationsByAgent
m_agentStationMap        -> m_stationAddressByAgent
m_stationOperations      -> m_downlinkSchedulesByStation
m_sortedOperations       -> m_uplinkSchedule
m_stationSockets         -> m_socketByStation
m_stationConnected       -> m_isConnectedByStation
m_stationSendEvents      -> m_sendEventByStation
m_perSecondStats         -> m_metricsByAbsoluteSecond
ops                      -> operations
gen                      -> stationGenerator
addr                     -> socketAddress
```

Do not rename public methods, trace sources, TypeIds, or logged component names.

- [ ] **Step 6: Reduce long methods and stale comments**

Extract only these cohesive private operations:

```cpp
void ConfigureSocket(const Address& stationAddress, Ptr<Socket> socket);
void RecordAcceptedSend(const ScheduledPayload& payload,
                        const Address& stationAddress,
                        uint32_t acceptedBytes,
                        Time txTime);
void ReportUnsentAgents() const;
```

Keep AP and STA variants separate where their state differs. Remove commented
logging alternatives and comments that repeat socket calls.

- [ ] **Step 7: Add complete Doxygen for touched declarations**

Document private member units with `///<`, callback purpose, event ownership,
and the reason schedules use the common epoch. Keep comments concise.

- [ ] **Step 8: Build and test both applications**

```bash
./ns3 build llm-test llm_sample
./test.py -s llm
```

Expected: PASS with no warnings.

- [ ] **Step 9: Commit generator cleanup**

```bash
git -C contrib/llm add model test
git -C contrib/llm commit -m "llm: Simplify traffic generators"
```

---

### Task 6: Extract traffic coordination

**Files:**
- Create: `examples/traffic-coordinator.h`
- Create: `examples/traffic-coordinator.cc`
- Create: `test/traffic-coordinator-test-suite.cc`
- Modify: `examples/sample-scenario.cc`
- Modify: `examples/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Modify: `test/llm-test-suite.cc`

**Interfaces:**
- Consumes: `APGenerator`, `StaLlmGenerator`, and `Application` references.
- Produces:

```cpp
int64_t GetNextIntegerSecondMs(int64_t nowMs);

class TrafficCoordinator
{
  public:
    TrafficCoordinator(double traceDurationMs, double maxExperimentDurationMs);
    Callback<void> GetReadyCallback();
    void AddGenerator(Ptr<APGenerator> generator);
    void AddGenerator(Ptr<StaLlmGenerator> generator);
    void AddApplication(Ptr<Application> application);
    void FinalizeRegistration();
    int64_t GetExperimentStartUs() const;
    double GetTraceDurationMs() const;
    double GetMaxExperimentDurationMs() const;

  private:
    void NotifyGeneratorReady();
};
```

- [ ] **Step 1: Add failing epoch tests**

Assert:

```cpp
NS_TEST_ASSERT_MSG_EQ(GetNextIntegerSecondMs(0), 1000, "Wrong epoch");
NS_TEST_ASSERT_MSG_EQ(GetNextIntegerSecondMs(999), 1000, "Wrong epoch");
NS_TEST_ASSERT_MSG_EQ(GetNextIntegerSecondMs(1000), 2000, "Wrong epoch");
NS_TEST_ASSERT_MSG_EQ(GetNextIntegerSecondMs(1999), 2000, "Wrong epoch");
```

Add a coordinator case with no generators and assert `FinalizeRegistration()`
triggers the same no-generator invariant through the ns-3 fatal test facility
used by existing fatal test cases.

- [ ] **Step 2: Verify the coordinator API is absent**

```bash
./ns3 build llm-test
```

Expected: FAIL because `traffic-coordinator.h` does not exist.

- [ ] **Step 3: Move readiness state into the coordinator**

Move the current generator/application vectors, expected and ready counts,
trace duration, maximum duration, experiment epoch, and
`TrafficGeneratorReady()` logic into the class. Implement epoch calculation
exactly as:

```cpp
return ((nowMs / 1000) + 1) * 1000;
```

The callback must bind `NotifyGeneratorReady()` to `this`. The object remains
owned by `main()` and outlives the simulator.

- [ ] **Step 4: Replace global registration in the scenario**

Create one `TrafficCoordinator` in `main()`. Pass it by reference to the
existing `SetupApGroup()` temporarily. Replace every generator/application
push with `AddGenerator()` or `AddApplication()`, and use
`GetReadyCallback()` for both generator types.

- [ ] **Step 5: Update example and test CMake sources**

Add both coordinator files to the `llm_sample` `SOURCE_FILES`. Add
`examples/traffic-coordinator.cc` and the coordinator test file to
`TEST_SOURCES`, then remove the empty factory definition.

- [ ] **Step 6: Run tests and example build**

```bash
./ns3 build llm-test llm_sample
./test.py -s llm
```

Expected: PASS. Confirm `sample-scenario.cc` no longer declares generator,
application, readiness-count, duration, or epoch globals.

- [ ] **Step 7: Commit the coordinator**

```bash
git -C contrib/llm add CMakeLists.txt examples test
git -C contrib/llm commit -m "llm: Encapsulate traffic coordination"
```

---

### Task 7: Encapsulate Wi-Fi statistics and JSON output

**Files:**
- Create: `examples/wifi-statistics.h`
- Create: `examples/wifi-statistics.cc`
- Create: `examples/wifi-statistics-json.cc`
- Create: `test/wifi-statistics-test-suite.cc`
- Modify: `examples/sample-scenario.cc:106-1518`
- Modify: `examples/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Modify: `test/llm-test-suite.cc`

**Interfaces:**
- Consumes: `TrafficCoordinator`, Wi-Fi devices, interface containers, and
  current AP/STA application trace arguments.
- Produces:

```cpp
struct PhyRateAccumulator
{
    uint64_t txAttempts{0};
    long double weightedRateBpsUs{0.0};
    long double airtimeUs{0.0};

    void Add(double rateBps, double allocatedAirtimeUs);
    void Merge(const PhyRateAccumulator& other);
    double AverageMbps() const;
};

bool GetStatisticsWindowIndex(int64_t absoluteUs,
                              int64_t experimentStartUs,
                              double maxExperimentDurationMs,
                              uint32_t windowMs,
                              uint32_t& windowIndex);

class WifiStatistics
{
  public:
    explicit WifiStatistics(const TrafficCoordinator& coordinator);
    void RegisterApGroup(int bssIndex,
                         Ipv4Address apAddress,
                         const Ipv4InterfaceContainer& stationInterfaces);
    void RegisterWifiDevice(uint32_t nodeId,
                            std::string nodeLabel,
                            Ptr<NetDevice> device);
    void RecordApTx(uint32_t nodeId,
                    Address stationAddress,
                    std::string agentKey,
                    uint32_t payloadBytes,
                    Time txTime);
    void RecordStaTx(uint32_t nodeId,
                     std::string agentKey,
                     uint32_t payloadBytes,
                     Time txTime);
    void RecordApTxDrop(uint32_t nodeId,
                        Address stationAddress,
                        std::string agentKey,
                        uint32_t payloadBytes,
                        Time txTime);
    void RecordStaTxDrop(uint32_t nodeId,
                         std::string agentKey,
                         uint32_t payloadBytes,
                         Time txTime);
    void WriteJson(const std::string& outputPath) const;
    void PrintCrossLayerReport() const;
};
```

- [ ] **Step 1: Add failing pure-statistics tests**

Assert window behavior for a 10 ms window:

```cpp
uint32_t index = 999;
NS_TEST_ASSERT_MSG_EQ(GetStatisticsWindowIndex(1000000, 1000000, 25.0, 10, index),
                      true,
                      "Epoch must be included");
NS_TEST_ASSERT_MSG_EQ(index, 0, "Wrong first window");
NS_TEST_ASSERT_MSG_EQ(GetStatisticsWindowIndex(1009999, 1000000, 25.0, 10, index),
                      true,
                      "Last microsecond of first window must be included");
NS_TEST_ASSERT_MSG_EQ(index, 0, "Wrong boundary window");
NS_TEST_ASSERT_MSG_EQ(GetStatisticsWindowIndex(1010000, 1000000, 25.0, 10, index),
                      true,
                      "Second window must be included");
NS_TEST_ASSERT_MSG_EQ(index, 1, "Wrong second window");
NS_TEST_ASSERT_MSG_EQ(GetStatisticsWindowIndex(1025000, 1000000, 25.0, 10, index),
                      false,
                      "End boundary must be excluded");
```

Add rates `10e6` for `100 us` and `20e6` for `300 us`; assert
`AverageMbps()` equals `17.5` within `1e-9`.

- [ ] **Step 2: Verify the statistics API is absent**

```bash
./ns3 build llm-test
```

Expected: FAIL because `wifi-statistics.h` does not exist.

- [ ] **Step 3: Move statistics state into one owner**

Move the current node-second records, delay/drop records, topology maps,
MAC/PHY windows, deduplication keys, and all callbacks from
`sample-scenario.cc` into `WifiStatistics`. Replace every `g_` access with a
member. Bind callbacks to the `WifiStatistics` instance owned by `main()`.

- [ ] **Step 4: Preserve all callback signatures and attribution rules**

Keep the exact current callback inputs, packet parsing, direction attribution,
window inclusion, PHY airtime calculation, tag deduplication, and log text.
Use the coordinator getters instead of duration and epoch globals.

- [ ] **Step 5: Separate JSON serialization**

Move `WriteMacStatsJson()` and its serialization-only helpers to
`wifi-statistics-json.cc` as `WifiStatistics::WriteJson()`. Collection methods
must not write files. Preserve every key, numeric unit, consistency flag, and
iteration order.

- [ ] **Step 6: Register example and test sources**

Add both statistics `.cc` files to the example. Add them and the test file to
`TEST_SOURCES`; remove the empty factory definition from the central suite.

- [ ] **Step 7: Run statistics and module checks**

```bash
./ns3 build llm-test llm_sample
./test.py -s llm
```

Expected: PASS. Confirm no statistics, node-label, topology-registration,
deduplication, duration, or epoch globals remain in `sample-scenario.cc`.

- [ ] **Step 8: Commit statistics ownership**

```bash
git -C contrib/llm add CMakeLists.txt examples test
git -C contrib/llm commit -m "llm: Encapsulate Wi-Fi statistics"
```

---

### Task 8: Extract traffic flow monitoring

**Files:**
- Create: `examples/traffic-flow-monitor.h`
- Create: `examples/traffic-flow-monitor.cc`
- Modify: `examples/sample-scenario.cc:61-104,1519-1692`
- Modify: `examples/CMakeLists.txt`

**Interfaces:**
- Consumes: `TrafficCoordinator` for the active epoch and `WifiStatistics` for
  current MAC payload attribution.
- Produces:

```cpp
class TrafficFlowMonitor
{
  public:
    TrafficFlowMonitor(const TrafficCoordinator& coordinator,
                       WifiStatistics& wifiStatistics);
    void ConnectDeviceTraces();
    void RecordDeviceTx(std::string context, Ptr<const Packet> packet);
    void RecordDeviceRx(std::string context, Ptr<const Packet> packet);
    void PrintTransmissionTimePerSender() const;
};
```

- [ ] **Step 1: Run the full module suite before moving callbacks**

```bash
./ns3 build llm-test llm_sample
./test.py -s llm
```

Expected: PASS.

- [ ] **Step 2: Move key and map ownership**

Move `TxRxKey`, its ordering/hash support, TX/RX timestamp maps, and byte totals
into `TrafficFlowMonitor`. Preserve the exact key fields and packet-size rules.

- [ ] **Step 3: Move callbacks and connection logic**

Move `DeviceTxTrace()`, `DeviceRxTrace()`, and
`PrintTransmissionTimePerSender()` into the class methods. Implement
`ConnectDeviceTraces()` with the same two `Config::Connect()` paths and bind
methods to `this`.

- [ ] **Step 4: Replace global callback use in main**

Construct `TrafficFlowMonitor` after the coordinator and statistics objects,
call `ConnectDeviceTraces()` at the same point in `main()`, and call its report
method after `Simulator::Run()` at the same point as before.

- [ ] **Step 5: Build and test**

```bash
./ns3 build llm-test llm_sample
./test.py -s llm
```

Expected: PASS. Confirm no traffic-flow maps remain in `sample-scenario.cc`.

- [ ] **Step 6: Commit flow-monitor ownership**

```bash
git -C contrib/llm add examples
git -C contrib/llm commit -m "llm: Encapsulate traffic monitoring"
```

---

### Task 9: Split scenario configuration and topology

**Files:**
- Create: `examples/scenario-config.h`
- Create: `examples/scenario-config.cc`
- Create: `examples/scenario-topology.h`
- Create: `examples/scenario-topology.cc`
- Create: `test/scenario-config-test-suite.cc`
- Modify: `examples/sample-scenario.cc:1755-2293`
- Modify: `examples/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Modify: `test/llm-test-suite.cc`

**Interfaces:**
- Consumes: distribution result, coordinator, statistics, and existing module
  applications.
- Produces:

```cpp
struct ScenarioConfig
{
    std::string tracePath;
    int bandwidthMhz{20};
    std::string statisticsOutputPath{"mac-node-stats.json"};
    bool automaticDuration{true};
    double fixedDurationMs{0.0};
    int bssCount{3};
    int stationsPerBss{30};
};

struct ScenarioArgumentResult
{
    bool valid{false};
    bool printUsage{false};
    ScenarioConfig config;
    std::string error;
};

ScenarioArgumentResult ParseScenarioArguments(
    const std::vector<std::string>& arguments);
void PrintScenarioUsage(std::ostream& output, const std::string& programName);

void SetupApGroup(int bssIndex,
                  int bandwidthMhz,
                  const std::map<std::string, Address>& stationAddressByAgent,
                  const std::map<std::string, std::vector<Operation>>& operationsByAgent,
                  Address apAddress,
                  uint32_t stationCount,
                  TrafficCoordinator& coordinator,
                  WifiStatistics& statistics);
```

- [ ] **Step 1: Add failing configuration tests**

Test these exact argument vectors, excluding the executable name:

```cpp
const auto minimal = ParseScenarioArguments({"trace.json"});
NS_TEST_ASSERT_MSG_EQ(minimal.valid, true, "Minimal arguments rejected");
NS_TEST_ASSERT_MSG_EQ(minimal.config.bandwidthMhz, 20, "Wrong default bandwidth");
NS_TEST_ASSERT_MSG_EQ(minimal.config.automaticDuration, true, "Wrong duration mode");

const auto fixed = ParseScenarioArguments(
    {"trace.json", "80", "stats.json", "2.5"});
NS_TEST_ASSERT_MSG_EQ(fixed.valid, true, "Fixed arguments rejected");
NS_TEST_ASSERT_MSG_EQ_TOL(fixed.config.fixedDurationMs,
                          2500.0,
                          1e-9,
                          "Wrong fixed duration");

const auto missing = ParseScenarioArguments({});
NS_TEST_ASSERT_MSG_EQ(missing.valid, false, "Missing trace accepted");
NS_TEST_ASSERT_MSG_EQ(missing.printUsage, true, "Usage not requested");

const auto invalidBandwidth = ParseScenarioArguments({"trace.json", "30"});
NS_TEST_ASSERT_MSG_EQ(invalidBandwidth.error,
                      "Unsupported bandwidth: 30 MHz. Expected 20, 40, 80 or 160.",
                      "Wrong bandwidth error");
```

Also test zero, negative, non-numeric, and partially numeric fixed-duration
strings against the current experiment-time error text.

- [ ] **Step 2: Verify the configuration API is absent**

```bash
./ns3 build llm-test
```

Expected: FAIL because `scenario-config.h` does not exist.

- [ ] **Step 3: Implement argument parsing without changing main output**

Move current defaults and validations into `ParseScenarioArguments()`. Keep
printing outside the parser. `main()` must print the same usage and error text,
then return `1` in the same cases. Preserve the current `std::stoi` behavior for
a non-numeric bandwidth argument rather than adding a new catch path.

- [ ] **Step 4: Move AP-group construction**

Move `StaAssociated()` and `SetupApGroup()` to `scenario-topology.cc`. Pass
coordinator and statistics references explicitly. Keep node counts, channel
objects, Wi-Fi standard, manager, SSIDs, mobility, addresses, ports,
application start times, trace bindings, and log text unchanged.

- [ ] **Step 5: Replace duplicate tuple conversion in topology setup**

Add one local pure conversion function in `scenario-topology.cc`:

```cpp
LegacyAgentOperations ConvertOperations(
    const std::map<std::string, std::vector<Operation>>& operationsByAgent);
```

Use the same converted map for AP and STA generator setup. Do not export this
function unless a second module consumer appears.

- [ ] **Step 6: Reduce `sample-scenario.cc` to orchestration**

The final entry point must contain only seed/default setup, argument handling,
parse/distribute calls, object construction, AP-group loop, trace connection,
simulator run, reports, destroy, and completion output. Preserve the two TCP
default-setting locations and their current order.

- [ ] **Step 7: Register sources and tests**

Add all four scenario `.cc` files to the example. Add `scenario-config.cc` and
its test to `TEST_SOURCES`; remove the central empty factory definition.

- [ ] **Step 8: Build and run unit tests**

```bash
./ns3 build llm-test llm_sample
./test.py -s llm
```

Expected: PASS. Confirm `sample-scenario.cc` contains no file-level mutable
state and is substantially below 500 lines.

- [ ] **Step 9: Commit scenario decomposition**

```bash
git -C contrib/llm add CMakeLists.txt examples test
git -C contrib/llm commit -m "llm: Split scenario responsibilities"
```

---

### Task 10: Add smoke coverage and complete quality cleanup

**Files:**
- Create: `test/examples-to-run.py`
- Modify: all touched `model/*.h`, `model/*.cc`, `examples/*.h`, and
  `examples/*.cc` files as required by style and Doxygen checks
- Modify: `CMakeLists.txt`
- Modify: `examples/CMakeLists.txt`

**Interfaces:**
- Consumes: the completed refactor and `test/data/minimal-trace.json`.
- Produces: registered smoke coverage, clean style output, and final verified
  build/test state.

- [ ] **Step 1: Register the smoke example**

Create `test/examples-to-run.py`:

```python
#! /usr/bin/env python3

cpp_examples = [
    (
        "llm_sample contrib/llm/test/data/minimal-trace.json "
        "20 build/llm-smoke-stats.json 0.2",
        "True",
        "False",
    ),
]

python_examples = []
```

- [ ] **Step 2: Run the registered smoke test**

```bash
./test.py -e llm_sample
test -s build/llm-smoke-stats.json
```

Expected: the example passes and produces non-empty statistics JSON under the
ignored build directory.

- [ ] **Step 3: Complete naming and comment cleanup**

Search for remaining vague identifiers and obsolete comments:

```bash
rg -n "\b(gen|ops|idx|info|dist)\b|TO""DO|FIX""ME|^// ={10,}" contrib/llm/model contrib/llm/examples
```

For each match, use a domain name or remove stale prose. Retain only comments
that explain units, ownership, callback lifetime, invariants, or non-obvious
ns-3 behavior.

- [ ] **Step 4: Complete Doxygen coverage**

Check every new or touched public class, method, parameter, return value, and
member. Use C-style Javadoc blocks and `///<` member comments. Do not document
overridden methods that inherit sufficient parent documentation.

- [ ] **Step 5: Format with a supported clang-format**

Install the supported formatter if it is absent:

```bash
sudo apt install -y clang-format-20
```

Then run:

```bash
./utils/check-style-clang-format.py --fix contrib/llm/model contrib/llm/examples contrib/llm/test
./utils/check-style-clang-format.py contrib/llm/model contrib/llm/examples contrib/llm/test
```

Expected: the second command exits `0` with no listed violations.

- [ ] **Step 6: Run final configure and targeted build**

```bash
./ns3 configure --enable-examples --enable-tests --enable-logs --enable-warnings --enable-werror
./ns3 build llm-test llm_sample
```

Expected: PASS with runtime logging enabled and no compiler warnings.

- [ ] **Step 7: Run final tests**

```bash
./test.py -s llm
./test.py -e llm_sample
```

Expected: both commands pass with zero failures or crashes.

- [ ] **Step 8: Verify compatibility and repository cleanliness**

```bash
./ns3 show targets | rg "llm|llm_sample"
git -C contrib/llm diff --check
git -C contrib/llm status --short
wc -l contrib/llm/model/*.cc contrib/llm/examples/*.cc
```

Expected: both targets are present, whitespace check is clean, only intended
refactor files are modified, `sample-scenario.cc` is below 500 lines, and no
new implementation file exceeds 600 lines without a documented cohesive
reason.

- [ ] **Step 9: Commit final smoke and quality changes**

```bash
git -C contrib/llm add CMakeLists.txt examples model test
git -C contrib/llm commit -m "llm: Complete module refactor"
```

- [ ] **Step 10: Review the complete commit range**

Run:

```bash
git -C contrib/llm log --oneline cdc5a32..HEAD
git -C contrib/llm diff --stat cdc5a32..HEAD
```

Expected: ten focused implementation commits or fewer only where adjacent
tasks were inseparable, with no unrelated files changed.
