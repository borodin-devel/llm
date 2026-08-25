# Unified Window Statistics Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace fragmented one-second and layer-specific statistics with one configurable-window, AP-BSS-parent/STA-child hierarchy covering general, APP, TCP, MAC, and PHY data in `output.json`.

**Architecture:** A generalized experiment-statistics owner receives every trace on one `statistics.window_ms` clock, stores sparse local entity accumulators, integrates per-peer TCP state, and builds BSS-parent plus STA-child window/overall summaries. A streaming serializer writes the fixed hierarchy and registry-derived metadata without a large JSON DOM. Transitional legacy state is allowed only until the final serializer cutover.

**Tech Stack:** C++23, ns-3 trace callbacks, CMake, `nlohmann::json` scalar encoding, ns-3 `TestSuite`, Python 3 live-verification script.

**Spec:** `docs/superpowers/specs/2026-08-25-unified-window-statistics-design.md`

## Global Constraints

- Keep `schema_version` equal to integer `1`; no compatibility aliases or migration layer.
- All interval measurements use `statistics.window_ms`; no final AP/STA private one-second statistics map remains.
- JSON is window-first: sparse `windows[]`, AP BSS-parent aggregates, STA child detail, parallel dense `overall`.
- Every emitted/overall entity has fixed `general_stats`, `app_stats`, `tcp_stats`, `mac_stats`, and `phy_stats`; every category has `uplink`/`downlink`, while non-directional PHY busy/utilization remains category-level.
- AP rows aggregate their BSS; station rows are child detail. Validate child sums only when attribution is complete.
- APP sender bytes are actual socket-accepted bytes. Receiver bytes/counts/inter-arrival come from `TrafficSink`.
- TCP metrics are per peer. CWND is time-weighted across windows; RTT/delay/inter-arrival are sample-weighted.
- Device matches belong to the transmit window and use matched estimated bytes for effective throughput.
- Never average window averages; merge raw totals/accumulators for `overall`.
- Undefined derived values are JSON null; known zero throughput with a valid duration is numeric zero.
- Keep effective configuration 8 sections/36 fields, dense entity inventory, metadata last, and no resolved paths.
- Preserve exclusive `output.json` no-clobber creation and body/flush/close error propagation.
- Remove obsolete `wifi_windows`, `wifi_summary`, `transmission_summary`, and `cross_layer_summary` roots and final report logs.
- Large window/entity arrays remain streaming; do not construct the complete output DOM.
- No non-vendored implementation or test file exceeds 600 lines; new public APIs have complete ns-3 Doxygen and ASCII comments.
- Update English/Russian docs together; do not modify trace data or outer user files/artifacts.
- Final verification runs every `contrib/llm/traces/*.json` exactly once under the approved duration matrix with no leftovers.
- Configure/build with examples, tests, logs, warnings, and warnings-as-errors.

---

### Task 1: Common entity registry and window primitives

**Files:**
- Create: `examples/experiment-statistics-types.h`
- Create: `examples/experiment-statistics-window.cc`
- Modify: `examples/wifi-statistics-internal.h`
- Modify: `examples/wifi-statistics.h`
- Modify: `examples/wifi-statistics-owner.cc`
- Modify: `examples/scenario-topology.cc`
- Modify: `examples/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Create: `test/experiment-window-test-suite.cc`
- Modify: `test/llm-test-suite.h`
- Modify: `test/llm-test-suite.cc`

**Interfaces:**
- Produces stable identities, directions, sample accumulators, window resolution/splitting, and inventory registration consumed by Tasks 2-7.
- The public class remains temporarily named `WifiStatistics` so the current serializer stays buildable; Task 7 performs the final no-alias rename.

- [ ] **Step 1: Write failing identity/window tests**

Create tests for these exact types/functions:

```cpp
enum class ExperimentEntityKind
{
    ACCESS_POINT,
    STATION
};

enum class ExperimentDirection
{
    UPLINK,
    DOWNLINK
};

struct ExperimentEntityIdentity
{
    ExperimentEntityKind kind;
    uint32_t accessPointId;
    std::optional<uint32_t> stationIndex;
    uint32_t nodeId;
    std::string nodeLabel;
    std::string ipv4;
};

struct ExperimentWindowBounds
{
    uint64_t index;
    int64_t startUs;
    int64_t durationUs;
};
```

The test registers AP 0/node 7/`10.1.0.1`, STA 0/node 8/`10.1.0.2`, and STA
1/node 9/`10.1.0.3`. Assert deterministic AP/STA inventory order and lookups by
node/IP.

For 25 ms windows and 60 ms experiment duration, assert:

```text
relative 0 us       -> window 0 [0, 25000), duration 25000
relative 24999 us   -> window 0
relative 25000 us   -> window 1 [25000, 50000), duration 25000
relative 59999 us   -> window 2 [50000, 60000), duration 10000
relative 60000 us   -> rejected
```

Assert index `2^32` resolves without narrowing. Split `[20000, 55000)` into
pieces `(window 0, 5000 us)`, `(window 1, 25000 us)`, `(window 2, 5000 us)`.

- [ ] **Step 2: Build and verify RED**

Register `CreateExperimentWindowTestCases()` and run from the outer root:

```bash
./ns3 build llm-test
```

Expected: fails because the types, registry, and helper functions do not exist.

- [ ] **Step 3: Implement the focused foundation types**

In `experiment-statistics-types.h`, define the exact types above plus:

```cpp
struct SampleAccumulator
{
    uint64_t count{0};
    long double sum{0.0};
    long double sumSquares{0.0};
    double minimum{std::numeric_limits<double>::infinity()};
    double maximum{0.0};

    void Add(double value);
    void Merge(const SampleAccumulator& other);
};

template <typename T>
struct DirectionPair
{
    T uplink;
    T downlink;

    T& Get(ExperimentDirection direction);
    const T& Get(ExperimentDirection direction) const;
};

struct LocalEntityWindowAccumulator
{
    // Tasks 2-5 add one focused category at a time.
};

using UnifiedExperimentWindowStore =
    std::map<uint64_t, std::map<uint32_t, LocalEntityWindowAccumulator>>;

class ExperimentEntityRegistry
{
  public:
    void RegisterAccessPoint(uint32_t accessPointId,
                             uint32_t nodeId,
                             std::string nodeLabel,
                             std::string ipv4);
    void RegisterStation(uint32_t accessPointId,
                         uint32_t stationIndex,
                         uint32_t nodeId,
                         std::string nodeLabel,
                         std::string ipv4);
    const ExperimentEntityIdentity* FindByNodeId(uint32_t nodeId) const;
    const ExperimentEntityIdentity* FindByIpv4(std::string_view ipv4) const;
    const std::vector<ExperimentEntityIdentity>& GetAccessPoints() const;
    const std::vector<ExperimentEntityIdentity>& GetStations() const;
};
```

Reject duplicate node IDs/IPs/identity indexes with `std::invalid_argument`.
Keep vectors sorted at insertion or finalize deterministically before exposure.

- [ ] **Step 4: Implement pure window helpers**

Declare/implement:

```cpp
bool ResolveExperimentWindow(int64_t relativeUs,
                             int64_t experimentDurationUs,
                             int64_t windowUs,
                             ExperimentWindowBounds& bounds);

std::vector<std::pair<uint64_t, int64_t>> SplitExperimentInterval(
    int64_t relativeStartUs,
    int64_t relativeEndUs,
    int64_t experimentDurationUs,
    int64_t windowUs);
```

Use subtraction/division that cannot evaluate `duration + width - 1`.
Clip interval endpoints to `[0, experimentDurationUs]`; return no pieces for
empty/out-of-range input.

- [ ] **Step 5: Add registry/window state without removing legacy state**

Append to the private state:

```cpp
ExperimentEntityRegistry entityRegistry;
UnifiedExperimentWindowStore unifiedWindows;
```

Reuse existing `windowMs/windowUs/coordinator`. Add temporary public registration
methods to `WifiStatistics`:

```cpp
void RegisterAccessPointIdentity(uint32_t accessPointId,
                                 uint32_t nodeId,
                                 std::string nodeLabel,
                                 Ipv4Address ipv4);
void RegisterStationIdentity(uint32_t accessPointId,
                             uint32_t stationIndex,
                             uint32_t nodeId,
                             std::string nodeLabel,
                             Ipv4Address ipv4);
```

Update topology setup to call them exactly once per entity while retaining
existing legacy registration until Task 7.

- [ ] **Step 6: Build, test, and commit**

```bash
./ns3 build llm-test llm_sample
./test.py -s llm --no-build
./utils/check-style-clang-format.py \
  contrib/llm/examples/experiment-statistics-types.h \
  contrib/llm/examples/experiment-statistics-window.cc \
  contrib/llm/test/experiment-window-test-suite.cc
git -C contrib/llm diff --check
git -C contrib/llm add CMakeLists.txt examples test
git -C contrib/llm commit -m "llm: Add unified statistics foundations"
```

---

### Task 2: Windowed application and sink statistics

**Files:**
- Modify: `examples/experiment-statistics-types.h`
- Create: `examples/experiment-statistics-app.cc`
- Modify: `examples/wifi-statistics-internal.h`
- Modify: `examples/wifi-statistics.h`
- Modify: `examples/wifi-statistics.cc`
- Modify: `examples/scenario-topology.cc`
- Modify: `model/ap-generator.h`
- Modify: `model/ap-generator.cc`
- Delete: `model/ap-generator-report.cc`
- Modify: `model/sta-llm-generator.h`
- Modify: `model/sta-llm-generator.cc`
- Delete: `model/sta-llm-generator-report.cc`
- Modify: `model/traffic-sink.h`
- Modify: `model/traffic-sink.cc`
- Modify: `examples/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Create: `test/experiment-app-test-suite.cc`
- Modify: `test/llm-test-suite.h`
- Modify: `test/llm-test-suite.cc`

**Interfaces:**
- Consumes Task 1 identity/window primitives.
- Produces per-window APP direction/agent/peer/IAT accumulators and trace connections used by Tasks 6-7.

- [ ] **Step 1: Define and test exact application accumulators**

Append:

```cpp
struct AppAgentAccumulator
{
    uint64_t acceptedSendCount{0};
    uint64_t acceptedPayloadBytes{0};
    uint64_t dropEventCount{0};
    uint64_t droppedPayloadBytes{0};
};

struct AppPeerAccumulator
{
    uint64_t acceptedSendCount{0};
    uint64_t acceptedPayloadBytes{0};
    uint64_t receiveEventCount{0};
    uint64_t receivedPayloadBytes{0};
    uint64_t dropEventCount{0};
    uint64_t droppedPayloadBytes{0};
};

struct AppDirectionAccumulator
{
    uint64_t acceptedSendCount{0};
    uint64_t acceptedPayloadBytes{0};
    uint64_t receiveEventCount{0};
    uint64_t receivedPayloadBytes{0};
    uint64_t dropEventCount{0};
    uint64_t droppedPayloadBytes{0};
    SampleAccumulator receiveInterArrivalUs;
    std::map<std::string, AppAgentAccumulator> agents;
    std::map<uint32_t, AppPeerAccumulator> peersByNodeId;
};
```

Append to `LocalEntityWindowAccumulator`:

```cpp
DirectionPair<AppDirectionAccumulator> app;
```

Add private cross-window IAT state keyed by `(nodeId, direction, peerNodeId)`:

```cpp
struct AppReceiveStreamKey
{
    uint32_t nodeId;
    ExperimentDirection direction;
    std::optional<uint32_t> peerNodeId;
    bool operator<(const AppReceiveStreamKey& other) const;
};

std::map<AppReceiveStreamKey, int64_t> lastApplicationReceiveTimeUs;
```

Create a test using 10 ms windows that records AP downlink accepted bytes for
two agents/STA peers, STA uplink accepted bytes, an AP uplink sink receive, a
STA downlink sink receive, and drops. Assert exact window ownership, actual
accepted byte values, deterministic maps, and no entry outside duration.

Add interleaved sink receives from two peers and prove IAT is calculated only
within each entity/direction/peer stream, then merged into direction totals.

- [ ] **Step 2: Build and verify RED**

Register `CreateExperimentAppTestCases()`:

```bash
./ns3 build llm-test
```

Expected: fails because APP accumulator/recording APIs do not exist.

- [ ] **Step 3: Add central recording APIs**

Declare/implement private pure callbacks plus public connections:

```cpp
void RecordAcceptedApplicationSend(uint32_t nodeId,
                                   ExperimentDirection direction,
                                   std::optional<uint32_t> peerNodeId,
                                   const std::string& agentKey,
                                   uint32_t acceptedBytes,
                                   int64_t absoluteTimeUs);
void RecordApplicationDrop(uint32_t nodeId,
                           ExperimentDirection direction,
                           std::optional<uint32_t> peerNodeId,
                           const std::string& agentKey,
                           uint32_t droppedBytes,
                           int64_t absoluteTimeUs);
void RecordApplicationReceive(uint32_t nodeId,
                              ExperimentDirection direction,
                              std::optional<uint32_t> peerNodeId,
                              uint32_t receivedBytes,
                              int64_t absoluteTimeUs);
```

Store APP accumulators under a new sparse unified window map by local node ID.
Keep old cross-layer fields updated temporarily where current output requires
them; delete that dual write in Task 7.

- [ ] **Step 4: Correct generator traces and remove one-second reports**

Remove both private maps/report methods/report source files. Change accepted
send trace calls to pass `acceptedBytes`, not requested `payloadBytes`.

Keep exact trace identity:

```text
AP: station Address, agent key, accepted bytes, transmit Time
STA: agent key, accepted bytes, transmit Time
```

Drop traces retain rejected remainder bytes. Do not print final per-second or
overall generator measurements during stop/dispose.

- [ ] **Step 5: Connect TrafficSink symmetrically**

Add `WifiStatistics::ConnectTrafficSink(Ptr<TrafficSink>, nodeId, direction)`.
Modify topology sink installation to connect AP sink as uplink receiver and
each STA sink as downlink receiver. Resolve the remote IP from `RxCustom`'s
`Address`; unknown peers still contribute direction totals but not peer maps.

Remove final `[Received Stats]` measurement logging and sink-owned IAT vector;
the central owner now owns IAT samples.

- [ ] **Step 6: Build, test, log-scan, and commit**

```bash
./ns3 build llm-test llm_sample
./test.py -s llm --no-build
! rg -n 'PrintPerSecondMetrics|m_metricsByAbsoluteSecond|Final per-second|Final overall|Received Stats' \
  contrib/llm/model
./utils/check-style-clang-format.py contrib/llm/model contrib/llm/examples contrib/llm/test
git -C contrib/llm diff --check
git -C contrib/llm add CMakeLists.txt examples model test
git -C contrib/llm commit -m "llm: Collect application statistics by window"
```

---

### Task 3: Per-peer TCP windows and time-weighted CWND

**Files:**
- Modify: `examples/experiment-statistics-types.h`
- Create: `examples/experiment-statistics-tcp.cc`
- Modify: `examples/wifi-statistics-internal.h`
- Modify: `examples/wifi-statistics.h`
- Modify: `examples/wifi-statistics.cc`
- Modify: `model/ap-generator.h`
- Modify: `model/ap-generator.cc`
- Modify: `model/sta-llm-generator.h`
- Modify: `model/sta-llm-generator.cc`
- Modify: `examples/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Create: `test/experiment-tcp-test-suite.cc`
- Modify: `test/llm-test-suite.h`
- Modify: `test/llm-test-suite.cc`

**Interfaces:**
- Produces per-owner/direction/peer TCP accumulators, current connection state, and `FinalizeTcpStatistics()` consumed by Task 6.

- [ ] **Step 1: Define TCP keys/state and write failing tests**

```cpp
struct TcpConnectionKey
{
    uint32_t ownerNodeId;
    ExperimentDirection direction;
    uint32_t peerNodeId;
    bool operator<(const TcpConnectionKey& other) const;
};

struct TcpWindowAccumulator
{
    long double congestionWindowBytesUs{0.0};
    uint64_t congestionWindowObservationDurationUs{0};
    std::optional<uint32_t> lastCongestionWindowBytes;
    SampleAccumulator roundTripTimeUs;
};

struct TcpConnectionState
{
    std::optional<uint32_t> currentCongestionWindowBytes;
    int64_t stateStartUs{0};
};
```

Store per-window TCP values under the local entity:

```cpp
std::map<std::pair<ExperimentDirection, uint32_t>, TcpWindowAccumulator> tcpConnections;
```

Store current step-function state once in the owner:

```cpp
std::map<TcpConnectionKey, TcpConnectionState> tcpConnectionStates;
bool tcpStatisticsFinalized{false};
```

Use 10 ms windows and 30 ms duration. Seed CWND 1000 bytes at -5 ms, change to
2000 at 5 ms, 4000 at 15 ms, then finalize at 30 ms. Assert:

```text
window 0: (1000*5000 + 2000*5000)/10000 = 1500 bytes
window 1: (2000*5000 + 4000*5000)/10000 = 3000 bytes
window 2: 4000 bytes
overall: (1000*5000 + 2000*10000 + 4000*15000)/30000
```

Use two AP downlink peers and prove states never combine. Add RTT samples to
different windows and assert per-peer sample count/sum/squares/min/max.

- [ ] **Step 2: Build and verify RED**

Register `CreateExperimentTcpTestCases()`:

```bash
./ns3 build llm-test
```

Expected: missing TCP accumulator/state/record/finalize APIs.

- [ ] **Step 3: Add generator transport trace sources**

Add exact TypeId trace sources to both generators:

```text
CongestionWindowSample(Address peer, uint32_t newCwndBytes, Time eventTime)
RoundTripTimeSample(Address peer, Time rtt, Time eventTime)
```

For AP sockets, bind the station address into both `CongestionWindow` and
newly connected `LastRTT` callbacks. For the STA socket, use its configured AP
remote address. Emit nonzero RTT samples only. The generators do not aggregate.

- [ ] **Step 4: Implement central TCP state integration**

Declare:

```cpp
void RecordCongestionWindow(uint32_t ownerNodeId,
                            ExperimentDirection direction,
                            uint32_t peerNodeId,
                            uint32_t newCwndBytes,
                            int64_t absoluteTimeUs);
void RecordRoundTripTime(uint32_t ownerNodeId,
                         ExperimentDirection direction,
                         uint32_t peerNodeId,
                         int64_t rttUs,
                         int64_t absoluteTimeUs);
void FinalizeTcpStatistics();
```

Before applying a new CWND, integrate the previous state over
`[max(stateStart, epoch), min(eventTime, experimentEnd))` using Task 1's
splitter. Events before epoch only replace cached state/start time. Finalize
flushes each state through experiment end and is idempotent; a second call
must not add duration twice.

Connect AP as downlink and STA as uplink. For the AP BSS-parent uplink view,
Task 6 later copies/merges child STA connection records; do not aggregate
connections in this task.

- [ ] **Step 5: Verify and commit**

```bash
./ns3 build llm-test llm_sample
./test.py -s llm --no-build
./utils/check-style-clang-format.py \
  contrib/llm/examples/experiment-statistics-tcp.cc \
  contrib/llm/model/ap-generator.cc \
  contrib/llm/model/sta-llm-generator.cc \
  contrib/llm/test/experiment-tcp-test-suite.cc
git -C contrib/llm diff --check
git -C contrib/llm add CMakeLists.txt examples model test
git -C contrib/llm commit -m "llm: Collect per-peer TCP window statistics"
```

---

### Task 4: Device matching and MAC window statistics

**Files:**
- Modify: `examples/experiment-statistics-types.h`
- Create: `examples/experiment-statistics-device.cc`
- Create: `examples/experiment-statistics-mac.cc`
- Modify: `examples/wifi-statistics-internal.h`
- Modify: `examples/wifi-statistics.h`
- Modify: `examples/wifi-statistics.cc`
- Modify: `examples/sample-scenario.cc`
- Delete: `examples/traffic-flow-monitor.h`
- Delete: `examples/traffic-flow-monitor-internal.h`
- Delete: `examples/traffic-flow-monitor.cc`
- Delete: `examples/traffic-flow-summary.cc`
- Modify: `examples/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Create: `test/experiment-device-mac-test-suite.cc`
- Delete: `test/traffic-flow-summary-test-suite.cc`
- Modify: `test/llm-test-suite.h`
- Modify: `test/llm-test-suite.cc`

**Interfaces:**
- Produces per-window general/device matching and MAC directional/peer accumulators. Removes `TrafficFlowMonitor` while temporarily preserving the old transmission DTO adapter for current JSON until Task 7.

- [ ] **Step 1: Define exact device/general/MAC accumulator types**

```cpp
struct DeviceTransmissionAccumulator
{
    uint64_t estimatedTransmittedTcpPayloadBytes{0};
    uint64_t estimatedMatchedTcpPayloadBytes{0};
    uint64_t matchedPacketCount{0};
    SampleAccumulator transmissionDurationUs;
};

struct MacPeerAccumulator
{
    uint64_t estimatedTransmitEventCount{0};
    uint64_t estimatedTransmittedTcpPayloadBytes{0};
    uint64_t estimatedReceiveEventCount{0};
    uint64_t estimatedReceivedTcpPayloadBytes{0};
};

struct MacDirectionAccumulator
{
    uint64_t estimatedTransmitEventCount{0};
    uint64_t estimatedTransmittedTcpPayloadBytes{0};
    uint64_t estimatedReceiveEventCount{0};
    uint64_t estimatedReceivedTcpPayloadBytes{0};
    uint64_t transmitDropCount{0};
    uint64_t transmitDropPacketBytes{0};
    uint64_t mpduDropCount{0};
    uint64_t mpduDropBytes{0};
    uint64_t dataFailureCount{0};
    uint64_t finalDataFailureCount{0};
    std::map<int, uint64_t> mpduDropsByReason;
    std::map<uint32_t, MacPeerAccumulator> peersByNodeId;
};
```

Attach exact members to each sparse local entity window accumulator:

```cpp
DirectionPair<DeviceTransmissionAccumulator> deviceTransmission;
DirectionPair<MacDirectionAccumulator> mac;
```

- [ ] **Step 2: Write failing parsing/matching/direction tests**

Use a narrow internal test seam accepting parsed observations so packet header
construction does not duplicate parser logic:

```cpp
struct ParsedDeviceTcpPayload
{
    std::string sourceIpv4;
    uint16_t sourcePort;
    std::string destinationIpv4;
    uint16_t destinationPort;
    uint32_t estimatedPayloadBytes;
};
```

Record STA TX uplink at 9.9 ms and matching AP RX at 10.1 ms. Assert the match,
duration, and matched bytes belong to window 0. Add AP TX downlink/STA RX,
unmatched TX, zero/negative-duration rejection, multiple identical flow
ordering, and total bytes above `INT_MAX`.

Assert AP/STA MAC direction mapping, peer maps, drop packet/MPDU bytes, reason
order, and data/final failures.

- [ ] **Step 3: Build and verify RED**

Register `CreateExperimentDeviceMacTestCases()`:

```bash
./ns3 build llm-test
```

Expected: missing unified device/MAC APIs.

- [ ] **Step 4: Move device trace ownership into statistics**

Move `ParseTcpPayload`, `FlowKey`, TX/RX timestamp queues, and device trace
connections from TrafficFlowMonitor. `WifiStatistics::ConnectDeviceTraces()`
connects once after topology exists. Record sender/receiver entity and peer
identity from IP registry.

TX immediately increments estimated transmitted bytes/event in its causal
window. RX increments estimated received bytes/event in its event window. At
summary finalization, pair flows in order and add positive duration/matched
bytes to the stored TX window and sender direction.

Keep a transitional `BuildTransmissionSummary()` adapter sourced from the new
raw matching state so the current Task 4 root serializer remains buildable;
remove it in Task 7.

- [ ] **Step 5: Move MAC drop/failure recording to configured windows**

Use event time and entity role to choose AP downlink or STA uplink for
transmitter-side drops/failures. Parse peer when the callback provides a
resolvable remote address; otherwise update only direction totals. Continue
writing legacy cross-layer fields temporarily for the existing serializer,
then remove dual writes in Task 7.

- [ ] **Step 6: Remove TrafficFlowMonitor and integrate main**

Delete its source/header/test files and CMake entries. Main calls
`wifiStatistics.ConnectDeviceTraces()` and later obtains the transitional
summary adapter from the same owner. No duplicate MacTx/MacRx connection may
remain.

- [ ] **Step 7: Build, test, and commit**

```bash
./ns3 build llm-test llm_sample
./test.py -s llm --no-build
! rg -n 'TrafficFlowMonitor' contrib/llm/examples contrib/llm/test contrib/llm/CMakeLists.txt
./utils/check-style-clang-format.py contrib/llm/examples contrib/llm/test
git -C contrib/llm diff --check
git -C contrib/llm add CMakeLists.txt examples test
git -C contrib/llm commit -m "llm: Unify device and MAC window statistics"
```

---

### Task 5: PHY and cross-layer statistics on configured windows

**Files:**
- Modify: `examples/experiment-statistics-types.h`
- Create: `examples/experiment-statistics-phy.cc`
- Modify: `examples/wifi-statistics-internal.h`
- Modify: `examples/wifi-statistics.h`
- Modify: `examples/wifi-statistics.cc`
- Modify: `examples/wifi-statistics-summary.cc`
- Modify: `examples/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Create: `test/experiment-phy-test-suite.cc`
- Modify: `test/cross-layer-summary-test-suite.cc`
- Modify: `test/wifi-statistics-test-suite.cc`
- Modify: `test/llm-test-suite.h`
- Modify: `test/llm-test-suite.cc`

**Interfaces:**
- Produces configured-window PHY and application-to-PHY accumulators plus AP/STA flow attribution. Legacy one-second/old-PHY state remains only until Task 7 cutover.

- [ ] **Step 1: Define PHY and cross-layer accumulator types**

```cpp
struct PhyPeerAccumulator
{
    uint64_t taggedPayloadBytes{0};
    uint64_t uniqueTaggedPayloadBytes{0};
    uint64_t transmissionAttemptCount{0};
    uint64_t retransmissionCount{0};
    long double dataRateBpsUs{0.0};
    long double transmissionAirtimeUs{0.0};
};

struct PhyDirectionAccumulator : PhyPeerAccumulator
{
    uint64_t taggedMpduCount{0};
    uint64_t completeTaggedMpduBytes{0};
    std::map<uint32_t, PhyPeerAccumulator> peersByNodeId;
};

struct PhyCategoryAccumulator
{
    int64_t busyTimeUs{0};
    PhyDirectionAccumulator uplink;
    PhyDirectionAccumulator downlink;
};
```

Append to each local entity window:

```cpp
DirectionPair<SampleAccumulator> applicationToPhyDelayUs;
PhyCategoryAccumulator phy;
```

- [ ] **Step 2: Write failing PHY/window attribution tests**

Use a 10 ms window state and literal tagged spans. Assert:

- STA uplink attempt updates STA uplink plus AP BSS-parent uplink attribution;
- AP downlink attempt updates AP downlink plus destination STA downlink;
- first MPDU transmission updates unique bytes, repeated identity updates
  attempted bytes/retransmission only;
- complete MPDU bytes/count are not duplicated per tagged span;
- peer totals exclude complete MPDU bytes/count because they remain
  direction-level once-per-MPDU values;
- two rates/airtimes produce the literal weighted average;
- no airtime means null later;
- app-to-PHY delay belongs to sender direction/window;
- busy interval `[8 ms, 22 ms)` splits 2/10/2 ms over windows 0/1/2;
- AP category busy time uses AP PHY only, never summed station time.

- [ ] **Step 3: Build and verify RED**

Register `CreateExperimentPhyTestCases()`:

```bash
./ns3 build llm-test
```

Expected: missing PHY unified accumulators/recorders.

- [ ] **Step 4: Record PHY attempts and delay by configured window**

Refactor existing tagged MPDU identity and rate allocation code to write the
new local/window records while temporarily retaining old state. Resolve source
and destination entities from tag IPs. A transmitter entity receives its local
direction record; the traffic peer receives attributed child detail; Task 6
builds AP parents from these raw local/attributed values.

Use the same first-transmission identity rule for unique bytes and
application-to-PHY delay. Preserve packet/PPDU attempt semantics and
airtime-by-tagged-byte allocation.

- [ ] **Step 5: Split PHY state intervals at configured boundaries**

Replace the fixed-second splitter for the new state with Task 1's generic
window splitter. Clip to experiment bounds. Continue legacy second state only
until Task 7, clearly labeled transitional in code comments.

- [ ] **Step 6: Adapt existing tests and verify**

Keep current public JSON/cross-layer tests passing through legacy adapters.
Move any pure PHY fixture from `wifi-statistics-test-suite.cc` into the new
focused suite if the old test file would exceed 600 lines.

```bash
./ns3 build llm-test llm_sample
./test.py -s llm --no-build
./utils/check-style-clang-format.py contrib/llm/examples contrib/llm/test
git -C contrib/llm diff --check
git -C contrib/llm add CMakeLists.txt examples test
git -C contrib/llm commit -m "llm: Collect PHY statistics by window"
```

---

### Task 6: BSS-parent windows, dense overall, and validation

**Files:**
- Create: `examples/experiment-window-output.h`
- Create: `examples/experiment-statistics-summary.cc`
- Create: `examples/experiment-statistics-validation.cc`
- Modify: `examples/experiment-statistics-types.h`
- Modify: `examples/wifi-statistics-internal.h`
- Modify: `examples/wifi-statistics.h`
- Modify: `examples/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Create: `test/experiment-summary-test-suite.cc`
- Create: `test/experiment-validation-test-suite.cc`
- Modify: `test/llm-test-suite.h`
- Modify: `test/llm-test-suite.cc`

**Interfaces:**
- Consumes unified raw accumulators from Tasks 2-5.
- Produces `UnifiedExperimentSummary BuildUnifiedExperimentSummary()` containing sparse windows, dense overall, inventory, and raw-total validation for Task 7.

- [ ] **Step 1: Define unified hierarchy output records**

Create typed records for:

```text
SampleDistributionOutput
GeneralDirectionOutput
AppAgentOutput, AppPeerOutput, AppDirectionOutput
TcpConnectionOutput, TcpDirectionOutput
MacPeerOutput, MacDropReasonOutput, MacDirectionOutput
PhyPeerOutput, PhyDirectionOutput, PhyCategoryOutput
EntityStatisticsOutput
AccessPointStatisticsOutput, StationStatisticsOutput
ExperimentWindowOutput
ExperimentOverallOutput
ExperimentValidationOutput
UnifiedExperimentSummary
```

Each C++ member uses camelCase matching every JSON field in the spec. Derived
values that can be undefined use `std::optional<double>`. Counts/bytes/indexes
use `uint64_t`; node/AP/station IDs use `uint32_t`; times/rates use `double`
except raw microsecond durations/counts.

`EntityStatisticsOutput` always owns two directions for each category and
category-level PHY busy/utilization. AP/STA records include exact identity.

Use these exact field inventories:

```text
SampleDistributionOutput:
  sampleCount, averageUs, standardDeviationUs, minimumUs, maximumUs

GeneralDirectionOutput:
  estimatedTransmittedTcpPayloadBytes, estimatedMatchedTcpPayloadBytes,
  matchedPacketCount, totalTransmissionDurationUs,
  averageTransmissionDurationUs, transmissionDurationStandardDeviationUs,
  minimumTransmissionDurationUs, maximumTransmissionDurationUs,
  effectiveThroughputMbps, applicationToPhyDelay

AppAgentOutput:
  agentKey, acceptedSendCount, acceptedPayloadBytes,
  acceptedThroughputMbps, acceptedBandwidthSharePercent,
  dropEventCount, droppedPayloadBytes

AppPeerOutput:
  peerNodeId, peerIpv4, acceptedSendCount, acceptedPayloadBytes,
  acceptedThroughputMbps, acceptedBandwidthSharePercent,
  receiveEventCount, receivedPayloadBytes, receivedThroughputMbps,
  receivedBandwidthSharePercent, dropEventCount, droppedPayloadBytes

AppDirectionOutput:
  acceptedSendCount, acceptedPayloadBytes, acceptedThroughputMbps,
  receiveEventCount, receivedPayloadBytes, receivedThroughputMbps,
  dropEventCount, droppedPayloadBytes, receiveInterArrivalTime,
  agents, peers

TcpConnectionOutput:
  peerNodeId, peerIpv4, congestionWindowObservationDurationUs,
  averageCongestionWindowBytes, lastCongestionWindowBytes, roundTripTime

TcpDirectionOutput:
  connections

MacDropReasonOutput:
  reasonCode, dropCount

MacPeerOutput:
  peerNodeId, peerIpv4, estimatedTransmitEventCount,
  estimatedTransmittedTcpPayloadBytes, estimatedTransmitThroughputMbps,
  estimatedReceiveEventCount, estimatedReceivedTcpPayloadBytes,
  estimatedReceiveThroughputMbps

MacDirectionOutput:
  estimatedTransmitEventCount, estimatedTransmittedTcpPayloadBytes,
  estimatedTransmitThroughputMbps, estimatedReceiveEventCount,
  estimatedReceivedTcpPayloadBytes, estimatedReceiveThroughputMbps,
  transmitDropCount, transmitDropPacketBytes, mpduDropCount,
  mpduDropBytes, dataFailureCount, finalDataFailureCount,
  mpduDropsByReason, peers

PhyPeerOutput:
  peerNodeId, peerIpv4, taggedPayloadBytes, uniqueTaggedPayloadBytes,
  transmissionAttemptCount, retransmissionCount,
  transmissionAirtimeUs, averageDataRateMbps, throughputMbps

PhyDirectionOutput:
  taggedPayloadBytes, uniqueTaggedPayloadBytes, taggedMpduCount,
  completeTaggedMpduBytes, transmissionAttemptCount, retransmissionCount,
  transmissionAirtimeUs, averageDataRateMbps, throughputMbps, peers

PhyCategoryOutput:
  busyTimeUs, channelUtilizationPercent, uplink, downlink
```

`EntityStatisticsOutput` contains direction pairs named `generalStats`,
`appStats`, `tcpStats`, and `macStats`, plus `phyStats`.

`AccessPointStatisticsOutput` contains `accessPointId`, `nodeId`, `nodeLabel`,
`ipv4`, and `statistics`. `StationStatisticsOutput` additionally contains
`stationIndex`.

```cpp
struct ExperimentWindowOutput
{
    uint64_t windowIndex;
    double windowStartMs;
    double windowDurationMs;
    std::vector<AccessPointStatisticsOutput> accessPoints;
    std::vector<StationStatisticsOutput> stations;
};

struct ExperimentOverallOutput
{
    std::vector<AccessPointStatisticsOutput> accessPoints;
    std::vector<StationStatisticsOutput> stations;
};

struct ExperimentValidationOutput
{
    bool entityInventoryReferencesValid;
    bool appAgentTotalsConsistent;
    bool appPeerTotalsConsistent;
    bool macPeerTotalsConsistent;
    bool phyPeerTotalsConsistent;
    bool apStationSenderTotalsConsistent;
    bool overallMatchesWindows;
    bool uniquePhyPayloadWithinTaggedPayload;
};

struct UnifiedExperimentSummary
{
    uint32_t statisticsWindowMs;
    std::vector<ExperimentWindowOutput> windows;
    ExperimentOverallOutput overall;
    ExperimentValidationOutput validation;
    std::vector<ExperimentEntityIdentity> accessPointInventory;
    std::vector<ExperimentEntityIdentity> stationInventory;
};
```

- [ ] **Step 2: Write failing sparse/fixed-shape/BSS tests**

Build raw windows for one AP and two STAs:

```text
window 0: STA0 uplink accepted 1000; STA1 uplink accepted 2000
window 1: AP downlink accepted 4000 to STA0; AP sink received 2500 uplink
window 2: empty
```

Assert window 2 omitted; inactive entities omitted; emitted entities contain
all five categories/two directions. Assert AP0 window0 uplink accepted bytes
3000 and child values 1000/2000. Assert AP0 window1 downlink 4000 with STA0
detail and AP receiver uplink 2500 separate.

Add TCP connections for two peers and prove arrays remain separate. Add AP and
station busy time and prove AP busy is AP-local reference, not child sum.

- [ ] **Step 3: Write failing overall/average/null tests**

Use 25 ms + 10 ms partial windows. Assert:

- every inventory entity appears overall, including an all-zero STA;
- throughput uses total experiment duration, not active-window count;
- sample distributions merge raw count/sum/squares/min/max;
- PHY rate merges rate-airtime products;
- utilization uses total AP-local busy time;
- CWND uses bytes-us/observed-us and separate peer states;
- agent/peer overall shares use total bytes;
- zero denominator/sample/airtime/CWND values are null;
- known zero throughput over valid duration is 0.0.

- [ ] **Step 4: Build and verify RED**

Register both new test factories:

```bash
./ns3 build llm-test
```

Expected: missing output records/summary/validation APIs.

- [ ] **Step 5: Implement local STA output and AP BSS-parent merge**

Finalize device matches and TCP state before summary. Build station outputs
from local/attributed accumulators. Build AP directions using these exact
rules:

```text
downlink sender: AP local accepted/TCP/MAC/PHY + destination STA attribution
downlink receiver: merge STA sink/MAC receive observations
uplink sender: merge child STA accepted/TCP/MAC/PHY
uplink receiver: AP sink/MAC receive observations
PHY busy/utilization: AP local PHY state only
```

Do not sum a child measurement twice when it is already stored as attributed
AP detail. Derive peers/agents in deterministic order after raw merging.

- [ ] **Step 6: Implement raw overall merge and derived values**

Merge accumulators from every configured window, including windows omitted
from JSON due to zero activity. Calculate rates/shares/distributions from raw
overall values using spec formulas. Use the exact actual duration for window
rates and experiment duration for overall.

- [ ] **Step 7: Implement validation flags**

Compute exact booleans:

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

Compare integers/raw long-double accumulators before JSON rounding. Do not
compare accepted and received bytes. Unit tests must force each flag false
independently using a private copied fixture without corrupting production
state.

- [ ] **Step 8: Build, test, size-check, and commit**

```bash
./ns3 build llm-test llm_sample
./test.py -s llm --no-build
./utils/check-style-clang-format.py contrib/llm/examples contrib/llm/test
wc -l contrib/llm/examples/*.cc contrib/llm/examples/*.h \
  contrib/llm/test/*.cc | sort -nr | head -25
git -C contrib/llm diff --check
git -C contrib/llm add CMakeLists.txt examples test
git -C contrib/llm commit -m "llm: Build hierarchical window summaries"
```

---

### Task 7: Streaming hierarchy serializer and final API cutover

**Files:**
- Create: `examples/experiment-statistics.h`
- Create: `examples/experiment-statistics-internal.h`
- Create: `examples/experiment-statistics-owner.cc`
- Create: `examples/experiment-statistics-json.cc`
- Create: `examples/experiment-statistics-json-general.cc`
- Create: `examples/experiment-statistics-json-app.cc`
- Create: `examples/experiment-statistics-json-tcp.cc`
- Create: `examples/experiment-statistics-json-mac.cc`
- Create: `examples/experiment-statistics-json-phy.cc`
- Create: `examples/experiment-statistics-json-entity.cc`
- Modify: `examples/experiment-output-internal.h`
- Modify: `examples/experiment-json.cc`
- Modify: `examples/sample-scenario.cc`
- Modify: `examples/scenario-topology.h`
- Modify: `examples/scenario-topology.cc`
- Delete: `examples/wifi-statistics.h`
- Delete: `examples/wifi-statistics-internal.h`
- Delete: `examples/wifi-statistics-owner.cc`
- Delete: `examples/wifi-statistics.cc`
- Delete: `examples/wifi-statistics-json.cc`
- Delete: `examples/wifi-statistics-summary.cc`
- Delete: `examples/experiment-output.h`
- Delete: `examples/transmission-summary-json.cc`
- Delete: `examples/cross-layer-summary-json.cc`
- Modify: `examples/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Create: `test/experiment-hierarchy-json-test-suite.cc`
- Modify: `test/experiment-json-test-suite.cc`
- Delete: `test/wifi-statistics-test-suite.cc`
- Delete: `test/cross-layer-summary-test-suite.cc`
- Modify: `test/llm-test-suite.h`
- Modify: `test/llm-test-suite.cc`

**Interfaces:**
- Consumes `UnifiedExperimentSummary` and registry configuration writer.
- Produces final `ExperimentStatistics` API and schema-version-1 window-first `output.json`; removes all transitional/legacy owner/state/DTO APIs.

- [ ] **Step 1: Write failing exact-root/hierarchy tests**

Create a literal summary with one 10 ms window, one AP, one STA, every category,
both directions, two peers/agents/reasons, overall entities, all validation
flags, inventory, and representative 8/36 config.

Call the planned writer and assert physical root order:

```text
schema_version
measurement_semantics
statistics_window_ms
windows
overall
validation
experiment_metadata
```

Assert exact window/identity/category/direction/field keys from the spec, null
undefined values, numeric known zeros, deterministic arrays, metadata last,
inventory/config cardinality, and no resolved paths.

Recursively reject old roots and old `one_second_intervals` fields outside the
configuration subtree.

- [ ] **Step 2: Write failing streaming/I/O tests**

Retain existing output collision/missing-parent/write-state coverage under the
new writer. Serialize 10,000 small synthetic sparse windows and parse the
result to assert exact count/order. Implementation review must also verify that
hierarchy writers accept `const UnifiedExperimentSummary&`, write directly to
`std::ostream`, and never construct a root/array `nlohmann::json`; the test
exercises scale while the interface/code establishes the no-second-DOM property.

- [ ] **Step 3: Build and verify RED**

Register `CreateExperimentHierarchyJsonTestCases()`:

```bash
./ns3 build llm-test
```

Expected: missing final owner/writer/hierarchy APIs.

- [ ] **Step 4: Implement focused hierarchy writers**

Split serialization helpers so no file exceeds 600 lines:

```text
experiment-statistics-json-general.cc
experiment-statistics-json-app.cc
experiment-statistics-json-tcp.cc
experiment-statistics-json-mac.cc
experiment-statistics-json-phy.cc
experiment-statistics-json-entity.cc
```

Each helper writes supplied typed values only; it never computes averages or
totals. Use scalar escaping for strings/nulls. Entity writer always emits all
five categories/two directions. Root writer streams sparse windows, dense
overall, validation, then configuration/inventory metadata.

- [ ] **Step 5: Preserve root I/O safety**

Open with:

```cpp
std::ofstream output(outputPath, std::ios::out | std::ios::noreplace);
```

Keep path-bearing errors for exclusive create, body write, flush, and close.
Do not leave a success banner after failure.

- [ ] **Step 6: Rename owner/state and remove transitional data paths**

Create final `ExperimentStatistics` and `ExperimentStatisticsState`, update
topology/main/tests, then delete every `WifiStatistics*` file/class reference.
Move already implemented focused sources to final experiment-statistics names
without changing algorithms.

Remove `NodeSecondStats/nodeSeconds`, `MacWindowStats/macWindows`, legacy
`phyWindows`, old transmission/cross-layer DTOs/adapters/builders, dual-write
branches, and old root serializer helpers. No compatibility alias remains.

- [ ] **Step 7: Integrate finalization and main output lifecycle**

Main performs after `Simulator::Run()`:

```cpp
experimentStatistics.Finalize();
experimentStatistics.WriteExperimentJson(resolvedPaths.outputFile.string(), config);
```

`Finalize()` is idempotent and completes device matching/TCP state before
summary/output creation. Keep existing catch -> `Simulator::Destroy()` ->
error -> return 1 behavior. Remove stale separate summary construction.

- [ ] **Step 8: Remove fixed-second/report/old-root artifacts and adapt tests**

Delete/replace obsolete Wi-Fi/cross-layer test factories after coverage moves
to Tasks 1–6 and hierarchy tests. Production scans must find none:

```bash
rg -n 'WifiStatistics|NodeSecondStats|nodeSeconds|wifi_windows|wifi_summary|transmission_summary|cross_layer_summary|one_second_intervals' \
  contrib/llm/examples contrib/llm/model
```

Also verify hierarchy writer sources use `nlohmann::json` only through the
scalar encoder and effective-configuration writer, never as a window/root
array container.

- [ ] **Step 9: Build, test, format, and commit**

```bash
./utils/check-style-clang-format.py --fix contrib/llm/examples contrib/llm/model contrib/llm/test
./utils/check-style-clang-format.py contrib/llm/examples contrib/llm/model contrib/llm/test
./ns3 build llm-test llm_sample
./test.py -s llm --no-build
git -C contrib/llm diff --check
wc -l contrib/llm/examples/*.cc contrib/llm/examples/*.h \
  contrib/llm/model/*.cc contrib/llm/test/*.cc | sort -nr | head -25
git -C contrib/llm add CMakeLists.txt examples model test
git -C contrib/llm commit -m "llm: Write unified window statistics JSON"
```

---

### Task 8: Documentation, live traces, and final verification

**Files:**
- Modify: `README.md`
- Modify: `README_RU.md`
- Create: `scripts/live_test_traces.py`

**Interfaces:**
- Consumes the final hierarchy/CLI.
- Produces synchronized docs and reproducible all-JSON-trace live verification.

- [ ] **Step 1: Replace English output documentation**

Document simply and completely: window-first sparse model; AP BSS parents and
STA children; roots/identities; fixed categories/directions; every field/unit;
null/zero rules; event ownership; duration splitting; partial windows; all
average/share formulas; dense overall; validation; inventory/config metadata;
schema 1 breaking replacement; removed roots/reports; live matrix/cleanup.

- [ ] **Step 2: Mirror Russian documentation**

Translate explanations naturally while keeping JSON keys, formulas, examples,
commands, URLs, and shared links identical. Remove stale one-second/current-
root descriptions from both files.

- [ ] **Step 3: Write the all-trace live verification tool**

`scripts/live_test_traces.py` runs from the outer root and discovers exactly
`contrib/llm/traces/*.json`. Use:

```python
POLICY = {
    "1W_high_load_1s.json": {"mode": "auto", "timeout_seconds": 900},
    "1W_high_load_10s.json": {"mode": "auto", "timeout_seconds": 3600},
    "1W_high_load_1m.json": {
        "mode": "fixed", "seconds": 1.0, "timeout_seconds": 1800
    },
    "1W_high_load_10m.json": {
        "mode": "fixed", "seconds": 1.0, "timeout_seconds": 1800
    },
}
```

Fail when discovered/policy names differ. For each trace:

1. run `scripts/find_window.py validate`;
2. create `/tmp/llm-trace-live.<safe-name>.<random>`;
3. run one `llm_sample` command with basic config, trace override, explicit
   temp run folder, and no output-name override;
4. add fixed flags only for bounded policies;
5. parse `output.json` and assert schema/root/nonempty windows/fixed entity
   shape/inventory references/overall/all validation flags/config trace/8x36/
   old-root absence/no legacy report markers;
6. remove the validated temp directory in `finally`.

Use each policy's exact timeout in sequential
`subprocess.run(..., check=False, timeout=timeout_seconds)`. On failure, print
trace, command, return code, and the last 200 console lines. One run per trace.

- [ ] **Step 4: Add pure script self-tests**

Create pure functions for policy coverage, command construction, JSON shape
validation, and cleanup. `--self-test` uses temporary fake traces/documents to
assert all four exact commands, missing/unknown trace failures, malformed
hierarchy/old root/bad entity/missing category/missing validation failures with
path-bearing messages, and cleanup on command/parse failure.

Do not register this potentially environment-sensitive Python tool in the ns-3
quick suite; Step 6 invokes its deterministic self-test explicitly.

- [ ] **Step 5: Run public minimal smoke and log-absence check**

Run minimal trace with default `output.json`; parse exact hierarchy. Enable
APGenerator/StaLlmGenerator/TrafficSink/SampleScenario warning logs and assert
legacy final banners/rows absent while ordinary completion remains.

- [ ] **Step 6: Run clean build/unit/registered verification**

```bash
./ns3 clean
./ns3 configure --enable-examples --enable-tests --enable-logs \
  --enable-warnings --enable-werror
./utils/check-style-clang-format.py contrib/llm/examples contrib/llm/model contrib/llm/test
./ns3 build llm-test llm_sample
./test.py -s llm
./test.py -e 'llm_sample*'
python3 contrib/llm/scripts/live_test_traces.py --self-test
```

- [ ] **Step 7: Run every real JSON trace exactly once**

```bash
python3 contrib/llm/scripts/live_test_traces.py
```

Record trace, policy, return code, wall time, output size, window count,
AP/STA inventory count, and validation flags. Expected: all four pass; 1s/10s
are full auto, 1m/10m are one-second bounded.

- [ ] **Step 8: Run parity, range, structure, and cleanup gates**

Assert EN/RU key/formula/example/command/link parity; no stale roots/one-second
claims; config 8/35/commented run folder; output metadata 8/36; no trace diff.

```bash
git -C contrib/llm diff --check
wc -l contrib/llm/examples/*.cc contrib/llm/examples/*.h \
  contrib/llm/model/*.cc contrib/llm/test/*.cc | sort -nr | head -30
git -C contrib/llm status --short
test -z "$(find /tmp -maxdepth 1 -type d -name 'llm-trace-live.*' -print -quit)"
```

Preserve/report pre-existing outer
`run/26-08-25_14-12-55/mac-node-stats.json`; verify SHA-256 remains
`bc62df0060a4b7be9b7d4b841c69a64753f34a65f5cdf1df4ddb779ba8703b2a`.

- [ ] **Step 9: Commit final docs/live verification**

```bash
git -C contrib/llm add README.md README_RU.md scripts/live_test_traces.py
git -C contrib/llm commit -m "llm: Document unified window statistics"
```

- [ ] **Step 10: Review complete range**

```bash
git -C contrib/llm log --oneline d353426..HEAD
git -C contrib/llm diff --stat d353426..HEAD
git -C contrib/llm diff --name-only d353426..HEAD | rg '^traces/' && exit 1 || true
```

Expected: one plan commit, eight focused implementation commits, plus any
reviewed fix commits; no trace-data modifications or outer source changes, and
clean nested status.
