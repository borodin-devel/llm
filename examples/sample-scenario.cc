// examples/sample-scenario.cc
// Sample ns-3 scenario: 3 APs x 3 stations, 802.11ax, TCP, separate YansWifiChannel per AP group
//
// Usage: ./sample-scenario <traces.json> [bandwidth_mhz] [stats_output.json] [experiment_time]
//   bandwidth_mhz: 20, 40, 80 or 160 (default: 20)
//   stats_output.json: MAC per-node statistics (default: mac-node-stats.json)
//   experiment_time: "auto" (JSON duration + 2s, default) or fixed seconds (> 0)
//

#include "scenario-log.h"
#include "traffic-coordinator.h"

#include "ns3/ap-generator.h"
#include "ns3/sta-llm-generator.h"
#include "ns3/traffic-sink.h"
#include "ns3/agent-distribution.h"
#include "ns3/contention-aware-agent-distribution.h"

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/mobility-module.h"
#include "ns3/wifi-module.h"
#include "ns3/applications-module.h"
#include "ns3/error-rate-model.h"
#include "ns3/yans-wifi-helper.h"

#include "ns3/wifi-mac-header.h"
#include "ns3/wifi-mpdu.h"
#include "ns3/wifi-phy.h"
#include "ns3/wifi-psdu.h"
#include "ns3/wifi-tx-vector.h"
#include "ns3/wifi-phy-state-helper.h"
#include "ns3/wifi-remote-station-manager.h"
#include "ns3/ipv4-header.h"
#include "ns3/tcp-header.h"

#include <iostream>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <tuple>
#include <string>
#include <vector>
#include <map>
#include <functional>
#include <fstream>
#include <iomanip>
#include <stdexcept>
#include <limits>
#include <set>
#include <sstream>

using namespace ns3;


// Agent map key: "id_type" (e.g. "1_GUI交互综合Agent")
using AgentMap = std::map<std::string, std::vector<Operation>>;


static LogComponent& g_log = llm_example::GetScenarioLog();

// ============================================================================
// Device TX/RX trace maps
// ============================================================================

struct TxRxKey
{
    std::string txSrcIp;
    uint16_t txSrcPort;
    std::string rxSrcIp;
    uint16_t rxSrcPort;
    uint32_t bytes;

    bool operator==(const TxRxKey& other) const
    {
        return txSrcIp == other.txSrcIp &&
               txSrcPort == other.txSrcPort &&
               rxSrcIp == other.rxSrcIp &&
               rxSrcPort == other.rxSrcPort &&
               bytes == other.bytes;
    }
};

struct TxRxKeyHash
{
    std::size_t operator()(const TxRxKey& k) const
    {
        std::size_t h1 = std::hash<std::string>{}(k.txSrcIp);
        std::size_t h2 = std::hash<uint16_t>{}(k.txSrcPort);
        std::size_t h3 = std::hash<std::string>{}(k.rxSrcIp);
        std::size_t h4 = std::hash<uint16_t>{}(k.rxSrcPort);
        std::size_t h5 = std::hash<uint32_t>{}(k.bytes);
        return h1 ^ (h2 << 1) ^ (h3 << 2) ^ (h4 << 3) ^ (h5 << 4);
    }
};

std::map<std::string, std::vector<uint64_t>> g_txBytes;

std::map<TxRxKey, std::vector<uint64_t>, std::function<bool(const TxRxKey&, const TxRxKey&)>> g_txMap(
    [](const TxRxKey& a, const TxRxKey& b) { return std::make_tuple(a.txSrcIp, a.txSrcPort, a.rxSrcIp, a.rxSrcPort, a.bytes) <
                                                      std::make_tuple(b.txSrcIp, b.txSrcPort, b.rxSrcIp, b.rxSrcPort, b.bytes); });

std::map<TxRxKey, std::vector<uint64_t>, std::function<bool(const TxRxKey&, const TxRxKey&)>> g_rxMap(
    [](const TxRxKey& a, const TxRxKey& b) { return std::make_tuple(a.txSrcIp, a.txSrcPort, a.rxSrcIp, a.rxSrcPort, a.bytes) <
                                                      std::make_tuple(b.txSrcIp, b.txSrcPort, b.rxSrcIp, b.rxSrcPort, b.bytes); });

// ============================================================================
// Fixed-window MAC/PHY statistics
// ============================================================================

static constexpr uint32_t kMacStatsWindowMs = 10;
static constexpr int64_t kMacStatsWindowUs =
    static_cast<int64_t>(kMacStatsWindowMs) * 1000;

static constexpr double kAutoExperimentTailMarginMs = 2000.0;

// Trace callbacks are free functions until their state moves into collectors.
static const TrafficCoordinator* g_trafficCoordinator = nullptr;

struct PhyRateAccumulator
{
    uint64_t txAttempts{0};
    long double weightedRateBpsUs{0.0};
    long double airtimeUs{0.0};

    void Add(double rateBps, double allocatedAirtimeUs)
    {
        if (rateBps <= 0.0 || allocatedAirtimeUs <= 0.0)
        {
            return;
        }

        ++txAttempts;
        weightedRateBpsUs +=
            static_cast<long double>(rateBps) * allocatedAirtimeUs;
        airtimeUs += allocatedAirtimeUs;
    }

    void Merge(const PhyRateAccumulator& other)
    {
        txAttempts += other.txAttempts;
        weightedRateBpsUs += other.weightedRateBpsUs;
        airtimeUs += other.airtimeUs;
    }

    double AverageMbps() const
    {
        return airtimeUs > 0.0
                   ? static_cast<double>(weightedRateBpsUs / airtimeUs / 1e6L)
                   : 0.0;
    }

    double AirtimeUs() const
    {
        return static_cast<double>(airtimeUs);
    }
};

struct MacWindowStats
{
    std::map<std::string, uint64_t> upBytes;
    std::map<std::string, uint64_t> downBytes;
    std::map<std::string, PhyRateAccumulator> upPhyRates;
    std::map<std::string, PhyRateAccumulator> downPhyRates;
};

// Topology registry used both for direction attribution and for emitting zeros.
std::vector<std::vector<std::string>> g_staIpsByAp;
std::map<std::string, int> g_apByIp;
std::map<std::string, int> g_staApByIp;

// Sparse storage: bucket index -> AP id -> bytes per STA/direction.
// MAC storage remains available for diagnostics/comparison.  The output JSON
// below is sourced from PhyTxBegin and therefore uses g_phyWindowStats.
std::map<uint32_t, std::map<int, MacWindowStats>> g_macWindowStats;
std::map<uint32_t, std::map<int, MacWindowStats>> g_phyWindowStats;

static std::string
Ipv4ToString(Ipv4Address address)
{
    std::ostringstream stream;
    address.Print(stream);
    return stream.str();
}

// ============================================================================
// Cross-layer / PHY observability
// ============================================================================

struct DelayAccumulator
{
    uint64_t count{0};
    long double sumUs{0.0};
    long double sumSquaresUs{0.0};
    double minUs{std::numeric_limits<double>::infinity()};
    double maxUs{0.0};

    void Add(double delayUs)
    {
        ++count;
        sumUs += delayUs;
        sumSquaresUs += static_cast<long double>(delayUs) * delayUs;
        minUs = std::min(minUs, delayUs);
        maxUs = std::max(maxUs, delayUs);
    }

    void Merge(const DelayAccumulator& other)
    {
        count += other.count;
        sumUs += other.sumUs;
        sumSquaresUs += other.sumSquaresUs;
        minUs = std::min(minUs, other.minUs);
        maxUs = std::max(maxUs, other.maxUs);
    }

    double MeanUs() const
    {
        return count == 0 ? 0.0 : static_cast<double>(sumUs / count);
    }

    double StdDevUs() const
    {
        if (count == 0)
        {
            return 0.0;
        }
        const long double mean = sumUs / count;
        const long double variance = std::max<long double>(
            0.0,
            sumSquaresUs / count - mean * mean);
        return std::sqrt(static_cast<double>(variance));
    }
};

struct AgentDropStats
{
    uint64_t events{0};
    uint64_t bytes{0};
};

struct NodeSecondStats
{
    DelayAccumulator appToPhy;
    uint64_t appTxEvents{0};
    uint64_t appTxBytes{0};
    uint64_t appDropEvents{0};
    uint64_t appDropBytes{0};
    std::map<std::string, AgentDropStats> appDropsByAgent;

    uint64_t phyTaggedMpduCount{0};
    uint64_t phyPayloadBytes{0};
    uint64_t phyUniquePayloadBytes{0};
    uint64_t phyMpduBytes{0};
    uint64_t phyRetransmissions{0};
    int64_t phyBusyUs{0};

    uint64_t macTxDrops{0};
    uint64_t macTxDropBytes{0};
    uint64_t macMpduDrops{0};
    uint64_t macMpduDropBytes{0};
    std::map<int, uint64_t> macMpduDropsByReason;
    uint64_t macDataFailures{0};
    uint64_t macFinalDataFailures{0};
};

std::map<uint32_t, std::map<uint32_t, NodeSecondStats>> g_nodeSecondStats;
std::map<uint32_t, std::string> g_nodeLabels;

// (node, addr1, addr2, sequence, fragment, first app packet uid)
using PhyMpduKey =
    std::tuple<uint32_t, std::string, std::string, uint16_t, uint8_t, uint64_t>;
std::set<PhyMpduKey> g_seenTaggedMpdus;

static bool
GetExperimentSecond(int64_t absoluteUs, uint32_t& second)
{
    if (g_trafficCoordinator->GetExperimentStartUs() < 0 || absoluteUs < g_trafficCoordinator->GetExperimentStartUs())
    {
        return false;
    }

    const int64_t relativeUs = absoluteUs - g_trafficCoordinator->GetExperimentStartUs();
    const int64_t experimentEndUs = static_cast<int64_t>(std::ceil(
        g_trafficCoordinator->GetMaxExperimentDurationMs() * 1000.0));
    if (relativeUs >= experimentEndUs)
    {
        return false;
    }

    second = static_cast<uint32_t>(relativeUs / 1000000);
    return true;
}

static NodeSecondStats*
GetNodeSecondStats(uint32_t nodeId, int64_t absoluteUs)
{
    uint32_t second = 0;
    if (!GetExperimentSecond(absoluteUs, second))
    {
        return nullptr;
    }
    return &g_nodeSecondStats[nodeId][second];
}

static std::string
MacToString(Mac48Address address)
{
    std::ostringstream stream;
    stream << address;
    return stream.str();
}

static bool
ResolvePhyFlow(const std::string& srcIp,
               const std::string& dstIp,
               int& apId,
               std::string& hostId,
               bool& uplink)
{
    const auto srcSta = g_staApByIp.find(srcIp);
    const auto dstAp = g_apByIp.find(dstIp);
    if (srcSta != g_staApByIp.end() &&
        dstAp != g_apByIp.end() &&
        srcSta->second == dstAp->second)
    {
        apId = srcSta->second;
        hostId = srcIp;
        uplink = true;
        return true;
    }

    const auto srcAp = g_apByIp.find(srcIp);
    const auto dstSta = g_staApByIp.find(dstIp);
    if (srcAp != g_apByIp.end() &&
        dstSta != g_staApByIp.end() &&
        srcAp->second == dstSta->second)
    {
        apId = srcAp->second;
        hostId = dstIp;
        uplink = false;
        return true;
    }

    return false;
}

static bool
GetPhyWindowIndex(int64_t nowUs, uint32_t& bucketIndex)
{
    if (g_trafficCoordinator->GetExperimentStartUs() < 0 || nowUs < g_trafficCoordinator->GetExperimentStartUs())
    {
        return false;
    }

    const int64_t relativeUs = nowUs - g_trafficCoordinator->GetExperimentStartUs();
    const int64_t statsEndUs = static_cast<int64_t>(std::ceil(
        g_trafficCoordinator->GetMaxExperimentDurationMs() * 1000.0));
    if (relativeUs >= statsEndUs)
    {
        return false;
    }

    bucketIndex = static_cast<uint32_t>(relativeUs / kMacStatsWindowUs);
    return true;
}

static void
RecordPhyStats(int64_t nowUs,
               const std::string& srcIp,
               const std::string& dstIp,
               uint32_t payloadBytes)
{
    if (payloadBytes == 0)
    {
        return;
    }

    uint32_t bucketIndex = 0;
    if (!GetPhyWindowIndex(nowUs, bucketIndex))
    {
        return;
    }

    int apId = -1;
    std::string hostId;
    bool uplink = false;
    if (!ResolvePhyFlow(srcIp, dstIp, apId, hostId, uplink))
    {
        return;
    }

    auto& stats = g_phyWindowStats[bucketIndex][apId];
    auto& bytesByHost = uplink ? stats.upBytes : stats.downBytes;
    bytesByHost[hostId] += payloadBytes;
}

static void
StaAppTxTrace(uint32_t nodeId,
              std::string agentKey,
              uint32_t bytes,
              Time time)
{
    (void)agentKey;
    if (auto* stats = GetNodeSecondStats(nodeId, time.GetMicroSeconds()))
    {
        ++stats->appTxEvents;
        stats->appTxBytes += bytes;
    }
}

static void
ApAppTxTrace(uint32_t nodeId,
             Address station,
             std::string agentKey,
             uint32_t bytes,
             Time time)
{
    (void)station;
    StaAppTxTrace(nodeId, std::move(agentKey), bytes, time);
}

static void
RecordAppTxDropTrace(uint32_t nodeId,
                     const std::string& agentKey,
                     uint32_t bytes,
                     Time time)
{
    if (auto* stats = GetNodeSecondStats(nodeId, time.GetMicroSeconds()))
    {
        ++stats->appDropEvents;
        stats->appDropBytes += bytes;
        auto& agentStats = stats->appDropsByAgent[agentKey];
        ++agentStats.events;
        agentStats.bytes += bytes;
    }
}

static void
StaAppTxDropTrace(uint32_t nodeId,
                  std::string agentKey,
                  uint32_t bytes,
                  Time time)
{
    RecordAppTxDropTrace(nodeId, agentKey, bytes, time);
}

static void
ApAppTxDropTrace(uint32_t nodeId,
                 Address station,
                 std::string agentKey,
                 uint32_t bytes,
                 Time time)
{
    (void)station;
    RecordAppTxDropTrace(nodeId, agentKey, bytes, time);
}

static void
MacTxDropTrace(uint32_t nodeId, Ptr<const Packet> packet)
{
    if (auto* stats = GetNodeSecondStats(nodeId, Simulator::Now().GetMicroSeconds()))
    {
        ++stats->macTxDrops;
        stats->macTxDropBytes += packet->GetSize();
    }
}

static void
MacDroppedMpduTrace(uint32_t nodeId,
                    WifiMacDropReason reason,
                    Ptr<const WifiMpdu> mpdu)
{
    if (auto* stats = GetNodeSecondStats(nodeId, Simulator::Now().GetMicroSeconds()))
    {
        ++stats->macMpduDrops;
        stats->macMpduDropBytes += mpdu ? mpdu->GetSize() : 0;
        ++stats->macMpduDropsByReason[static_cast<int>(reason)];
    }
}

static void
MacTxDataFailedTrace(uint32_t nodeId, Mac48Address remote)
{
    (void)remote;
    if (auto* stats = GetNodeSecondStats(nodeId, Simulator::Now().GetMicroSeconds()))
    {
        ++stats->macDataFailures;
    }
}

static void
MacTxFinalDataFailedTrace(uint32_t nodeId, Mac48Address remote)
{
    (void)remote;
    if (auto* stats = GetNodeSecondStats(nodeId, Simulator::Now().GetMicroSeconds()))
    {
        ++stats->macFinalDataFailures;
    }
}

static void
PhyTxBeginTrace(uint32_t nodeId, Ptr<const Packet> packet, double txPowerW)
{
    (void)txPowerW;

    const int64_t nowUs = Simulator::Now().GetMicroSeconds();
    auto* nodeStats = GetNodeSecondStats(nodeId, nowUs);
    if (!nodeStats)
    {
        return;
    }

    struct TaggedSpan
    {
        AppTxTag tag;
        uint32_t bytes;
    };
    std::vector<TaggedSpan> spans;

    auto iterator = packet->GetByteTagIterator();
    while (iterator.HasNext())
    {
        const auto item = iterator.Next();
        if (item.GetTypeId() != AppTxTag::GetTypeId())
        {
            continue;
        }

        AppTxTag tag;
        item.GetTag(tag);
        const uint32_t taggedBytes = item.GetEnd() - item.GetStart();
        if (taggedBytes > 0)
        {
            spans.push_back({tag, taggedBytes});
        }
    }

    // Management/control frames have no application byte tag and are ignored
    // for flow/delay accounting. They are still included in PHY busy time.
    if (spans.empty())
    {
        return;
    }

    ++nodeStats->phyTaggedMpduCount;
    nodeStats->phyMpduBytes += packet->GetSize();

    bool firstTransmission = true;
    WifiMacHeader wifiHeader;
    if (packet->PeekHeader(wifiHeader) > 0 && wifiHeader.IsData())
    {
        const PhyMpduKey key{
            nodeId,
            MacToString(wifiHeader.GetAddr1()),
            MacToString(wifiHeader.GetAddr2()),
            wifiHeader.GetSequenceNumber(),
            wifiHeader.GetFragmentNumber(),
            spans.front().tag.GetAppPacketUid()};
        firstTransmission = g_seenTaggedMpdus.insert(key).second;
        if (!firstTransmission)
        {
            ++nodeStats->phyRetransmissions;
        }
    }

    for (const auto& span : spans)
    {
        const std::string src = Ipv4ToString(span.tag.GetSource());
        const std::string dst = Ipv4ToString(span.tag.GetDestination());

        // The primary JSON reports actual tagged application payload observed
        // at PHY, so retransmitted payload is intentionally counted again.
        RecordPhyStats(nowUs, src, dst, span.bytes);
        nodeStats->phyPayloadBytes += span.bytes;

        if (firstTransmission)
        {
            nodeStats->phyUniquePayloadBytes += span.bytes;
            const int64_t delayUs = nowUs - span.tag.GetAppTxTimeUs();
            if (delayUs >= 0)
            {
                nodeStats->appToPhy.Add(static_cast<double>(delayUs));
            }
        }
    }
}

static void
PhyTxPsduBeginTrace(uint32_t nodeId,
                    WifiPhyBand band,
                    WifiConstPsduMap psduMap,
                    WifiTxVector txVector,
                    double txPowerW)
{
    (void)nodeId;
    (void)txPowerW;

    const int64_t nowUs = Simulator::Now().GetMicroSeconds();
    uint32_t bucketIndex = 0;
    if (!GetPhyWindowIndex(nowUs, bucketIndex))
    {
        return;
    }

    struct PpduFlowContribution
    {
        uint64_t taggedBytes{0};
        long double rateBpsTimesBytes{0.0};
    };

    // AP id, UL flag, host id.  A single PPDU attempt contributes at most one
    // phy_tx_attempt to each host/direction represented in this map.
    using PpduFlowKey = std::tuple<int, bool, std::string>;
    std::map<PpduFlowKey, PpduFlowContribution> contributions;
    uint64_t totalTaggedBytes = 0;

    for (const auto& [staId, psdu] : psduMap)
    {
        if (!psdu)
        {
            continue;
        }

        const double rateBps = static_cast<double>(
            txVector.GetMode(staId).GetDataRate(txVector, staId));

        for (const auto& mpdu : *PeekPointer(psdu))
        {
            if (!mpdu || !mpdu->GetHeader().IsData())
            {
                continue;
            }

            const Ptr<const Packet> payload = mpdu->GetPacket();
            auto iterator = payload->GetByteTagIterator();
            while (iterator.HasNext())
            {
                const auto item = iterator.Next();
                if (item.GetTypeId() != AppTxTag::GetTypeId())
                {
                    continue;
                }

                AppTxTag tag;
                item.GetTag(tag);
                const uint32_t taggedBytes = item.GetEnd() - item.GetStart();
                if (taggedBytes == 0)
                {
                    continue;
                }

                int apId = -1;
                std::string hostId;
                bool uplink = false;
                if (!ResolvePhyFlow(Ipv4ToString(tag.GetSource()),
                                    Ipv4ToString(tag.GetDestination()),
                                    apId,
                                    hostId,
                                    uplink))
                {
                    continue;
                }

                auto& contribution =
                    contributions[std::make_tuple(apId, uplink, hostId)];
                contribution.taggedBytes += taggedBytes;
                contribution.rateBpsTimesBytes +=
                    static_cast<long double>(rateBps) * taggedBytes;
                totalTaggedBytes += taggedBytes;
            }
        }
    }

    if (totalTaggedBytes == 0 || contributions.empty())
    {
        return;
    }

    // This is the actual duration of the complete transmitted PPDU, including
    // PHY preamble/header and A-MPDU aggregation effects.  For a PPDU carrying
    // multiple tagged flows, its airtime is attributed proportionally to the
    // tagged application payload bytes of each flow.
    const Time ppduDuration =
        WifiPhy::CalculateTxDuration(psduMap, txVector, band);
    const long double ppduAirtimeUs =
        static_cast<long double>(ppduDuration.GetNanoSeconds()) / 1000.0L;
    if (ppduAirtimeUs <= 0.0L)
    {
        return;
    }

    for (const auto& [key, contribution] : contributions)
    {
        const auto& [apId, uplink, hostId] = key;
        if (contribution.taggedBytes == 0)
        {
            continue;
        }

        const long double allocatedAirtimeUs =
            ppduAirtimeUs * contribution.taggedBytes / totalTaggedBytes;
        const double averageRateBps = static_cast<double>(
            contribution.rateBpsTimesBytes / contribution.taggedBytes);

        auto& stats = g_phyWindowStats[bucketIndex][apId];
        auto& ratesByHost = uplink ? stats.upPhyRates : stats.downPhyRates;
        ratesByHost[hostId].Add(
            averageRateBps,
            static_cast<double>(allocatedAirtimeUs));
    }
}

static void
PhyStateTrace(uint32_t nodeId, Time start, Time duration, WifiPhyState state)
{
    if (state != WifiPhyState::TX &&
        state != WifiPhyState::RX &&
        state != WifiPhyState::CCA_BUSY)
    {
        return;
    }

    if (g_trafficCoordinator->GetExperimentStartUs() < 0)
    {
        return;
    }

    int64_t intervalStartUs = std::max<int64_t>(
        start.GetMicroSeconds(),
        g_trafficCoordinator->GetExperimentStartUs());
    int64_t intervalEndUs = start.GetMicroSeconds() + duration.GetMicroSeconds();
    const int64_t experimentEndUs = g_trafficCoordinator->GetExperimentStartUs() +
        static_cast<int64_t>(std::ceil(g_trafficCoordinator->GetMaxExperimentDurationMs() * 1000.0));
    intervalEndUs = std::min(intervalEndUs, experimentEndUs);

    while (intervalStartUs < intervalEndUs)
    {
        const int64_t relativeUs = intervalStartUs - g_trafficCoordinator->GetExperimentStartUs();
        const uint32_t second = static_cast<uint32_t>(relativeUs / 1000000);
        const int64_t nextBoundaryUs =
            g_trafficCoordinator->GetExperimentStartUs() + (static_cast<int64_t>(second) + 1) * 1000000;
        const int64_t pieceEndUs = std::min(intervalEndUs, nextBoundaryUs);
        g_nodeSecondStats[nodeId][second].phyBusyUs += pieceEndUs - intervalStartUs;
        intervalStartUs = pieceEndUs;
    }
}

static void
RegisterWifiObservability(uint32_t nodeId,
                          const std::string& label,
                          Ptr<NetDevice> device)
{
    Ptr<WifiNetDevice> wifi = DynamicCast<WifiNetDevice>(device);
    NS_ABORT_MSG_IF(!wifi, "Observability target is not a WifiNetDevice");

    g_nodeLabels[nodeId] = label;

    wifi->GetPhy()->TraceConnectWithoutContext(
        "PhyTxBegin",
        MakeBoundCallback(&PhyTxBeginTrace, nodeId));
    wifi->GetPhy()->TraceConnectWithoutContext(
        "PhyTxPsduBegin",
        MakeBoundCallback(&PhyTxPsduBeginTrace,
                          nodeId,
                          wifi->GetPhy()->GetPhyBand()));
    wifi->GetPhy()->GetState()->TraceConnectWithoutContext(
        "State",
        MakeBoundCallback(&PhyStateTrace, nodeId));
    wifi->GetMac()->TraceConnectWithoutContext(
        "MacTxDrop",
        MakeBoundCallback(&MacTxDropTrace, nodeId));
    wifi->GetMac()->TraceConnectWithoutContext(
        "DroppedMpdu",
        MakeBoundCallback(&MacDroppedMpduTrace, nodeId));

    Ptr<WifiRemoteStationManager> manager = wifi->GetRemoteStationManager();
    manager->TraceConnectWithoutContext(
        "MacTxDataFailed",
        MakeBoundCallback(&MacTxDataFailedTrace, nodeId));
    manager->TraceConnectWithoutContext(
        "MacTxFinalDataFailed",
        MakeBoundCallback(&MacTxFinalDataFailedTrace, nodeId));
}

static void
PrintCrossLayerStats()
{
    NS_LOG_WARN("========== App -> PHY / reliability statistics ==========");

    const uint32_t totalSecondBuckets =
        g_trafficCoordinator->GetMaxExperimentDurationMs() > 0.0
            ? static_cast<uint32_t>(std::ceil(g_trafficCoordinator->GetMaxExperimentDurationMs() / 1000.0))
            : 0;
    const double experimentDurationSeconds = g_trafficCoordinator->GetMaxExperimentDurationMs() / 1000.0;

    // Iterate the topology registry, not only the sparse statistics map.  This
    // makes unused STAs visible too (all-zero rows are meaningful diagnostics).
    for (const auto& [nodeId, label] : g_nodeLabels)
    {
        DelayAccumulator overallDelay;
        uint64_t totalAppTxBytes = 0;
        uint64_t totalPhyPayload = 0;
        uint64_t totalUniquePayload = 0;
        uint64_t totalPhyMpduBytes = 0;
        uint64_t totalRetransmissions = 0;
        uint64_t totalMacDrops = 0;
        uint64_t totalMacDropBytes = 0;
        uint64_t totalMacMpduDrops = 0;
        uint64_t totalMacMpduDropBytes = 0;
        std::map<int, uint64_t> totalMacMpduDropsByReason;
        uint64_t totalMacFailures = 0;
        uint64_t totalMacFinalFailures = 0;
        uint64_t totalAppDrops = 0;
        uint64_t totalAppDropBytes = 0;
        int64_t totalBusyUs = 0;

        const auto nodeIt = g_nodeSecondStats.find(nodeId);

        for (uint32_t second = 0; second < totalSecondBuckets; ++second)
        {
            static const NodeSecondStats emptyStats;
            const NodeSecondStats* stats = &emptyStats;
            if (nodeIt != g_nodeSecondStats.end())
            {
                const auto secondIt = nodeIt->second.find(second);
                if (secondIt != nodeIt->second.end())
                {
                    stats = &secondIt->second;
                }
            }

            const double bucketStartSeconds = static_cast<double>(second);
            const double bucketDurationSeconds = std::max(
                0.0,
                std::min(1.0, experimentDurationSeconds - bucketStartSeconds));
            const double denominator = bucketDurationSeconds > 0.0
                                           ? bucketDurationSeconds
                                           : 1.0;

            const double appThroughputMbps =
                static_cast<double>(stats->appTxBytes) * 8.0 / 1e6 / denominator;
            const double throughputMbps =
                static_cast<double>(stats->phyPayloadBytes) * 8.0 / 1e6 / denominator;
            const double uniqueThroughputMbps =
                static_cast<double>(stats->phyUniquePayloadBytes) * 8.0 / 1e6 /
                denominator;
            const double channelUtilization =
                bucketDurationSeconds > 0.0
                    ? std::min(100.0,
                               static_cast<double>(stats->phyBusyUs) /
                                   (bucketDurationSeconds * 1e6) * 100.0)
                    : 0.0;

            NS_LOG_WARN("[Node stats] node=" << label
                        << " second=" << second
                        << " app_to_phy_count=" << stats->appToPhy.count
                        << " app_to_phy_mean_us=" << stats->appToPhy.MeanUs()
                        << " app_to_phy_stddev_us=" << stats->appToPhy.StdDevUs()
                        << " app_to_phy_min_us="
                        << (stats->appToPhy.count ? stats->appToPhy.minUs : 0.0)
                        << " app_to_phy_max_us=" << stats->appToPhy.maxUs
                        << " app_tx_mbps=" << appThroughputMbps
                        << " phy_payload_mbps=" << throughputMbps
                        << " phy_unique_payload_mbps=" << uniqueThroughputMbps
                        << " channel_utilization=" << channelUtilization << "%"
                        << " phy_retrans=" << stats->phyRetransmissions
                        << " mac_tx_drops=" << stats->macTxDrops
                        << " mac_tx_drop_bytes=" << stats->macTxDropBytes
                        << " mac_mpdu_drops=" << stats->macMpduDrops
                        << " mac_mpdu_drop_bytes=" << stats->macMpduDropBytes
                        << " mac_data_failed=" << stats->macDataFailures
                        << " mac_final_data_failed=" << stats->macFinalDataFailures
                        << " app_drop_events=" << stats->appDropEvents
                        << " app_drop_bytes=" << stats->appDropBytes);

            for (const auto& [reason, count] : stats->macMpduDropsByReason)
            {
                NS_LOG_WARN("[MAC MPDU drop] node=" << label
                            << " second=" << second
                            << " reason=" << reason
                            << " count=" << count);
            }

            for (const auto& [agentKey, drop] : stats->appDropsByAgent)
            {
                NS_LOG_WARN("[App drop] node=" << label
                            << " second=" << second
                            << " agent=\"" << agentKey << "\""
                            << " events=" << drop.events
                            << " bytes=" << drop.bytes);
            }

            overallDelay.Merge(stats->appToPhy);
            totalAppTxBytes += stats->appTxBytes;
            totalPhyPayload += stats->phyPayloadBytes;
            totalUniquePayload += stats->phyUniquePayloadBytes;
            totalPhyMpduBytes += stats->phyMpduBytes;
            totalRetransmissions += stats->phyRetransmissions;
            totalMacDrops += stats->macTxDrops;
            totalMacDropBytes += stats->macTxDropBytes;
            totalMacMpduDrops += stats->macMpduDrops;
            totalMacMpduDropBytes += stats->macMpduDropBytes;
            for (const auto& [reason, count] : stats->macMpduDropsByReason)
            {
                totalMacMpduDropsByReason[reason] += count;
            }
            totalMacFailures += stats->macDataFailures;
            totalMacFinalFailures += stats->macFinalDataFailures;
            totalAppDrops += stats->appDropEvents;
            totalAppDropBytes += stats->appDropBytes;
            totalBusyUs += stats->phyBusyUs;
        }

        NS_LOG_WARN("[Node overall] node=" << label
                    << " seconds=" << experimentDurationSeconds
                    << " app_to_phy_count=" << overallDelay.count
                    << " app_to_phy_mean_us=" << overallDelay.MeanUs()
                    << " app_to_phy_stddev_us=" << overallDelay.StdDevUs()
                    << " app_to_phy_min_us="
                    << (overallDelay.count ? overallDelay.minUs : 0.0)
                    << " app_to_phy_max_us=" << overallDelay.maxUs
                    << " app_tx_bytes=" << totalAppTxBytes
                    << " phy_payload_bytes=" << totalPhyPayload
                    << " phy_unique_payload_bytes=" << totalUniquePayload
                    << " phy_mpdu_bytes=" << totalPhyMpduBytes
                    << " avg_app_tx_mbps="
                    << (experimentDurationSeconds > 0.0
                            ? static_cast<double>(totalAppTxBytes) * 8.0 / 1e6 /
                                  experimentDurationSeconds
                            : 0.0)
                    << " avg_phy_payload_mbps="
                    << (experimentDurationSeconds > 0.0
                            ? static_cast<double>(totalPhyPayload) * 8.0 / 1e6 /
                                  experimentDurationSeconds
                            : 0.0)
                    << " avg_channel_utilization="
                    << (experimentDurationSeconds > 0.0
                            ? std::min(100.0,
                                       static_cast<double>(totalBusyUs) /
                                           (experimentDurationSeconds * 1e6) * 100.0)
                            : 0.0)
                    << "% phy_retrans=" << totalRetransmissions
                    << " mac_tx_drops=" << totalMacDrops
                    << " mac_tx_drop_bytes=" << totalMacDropBytes
                    << " mac_mpdu_drops=" << totalMacMpduDrops
                    << " mac_mpdu_drop_bytes=" << totalMacMpduDropBytes
                    << " mac_data_failed=" << totalMacFailures
                    << " mac_final_data_failed=" << totalMacFinalFailures
                    << " app_drop_events=" << totalAppDrops
                    << " app_drop_bytes=" << totalAppDropBytes);

        for (const auto& [reason, count] : totalMacMpduDropsByReason)
        {
            NS_LOG_WARN("[MAC MPDU drop overall] node=" << label
                        << " reason=" << reason
                        << " count=" << count);
        }
    }

    NS_LOG_WARN("==========================================================");
}

static void
RegisterApGroupForMacStats(int apIndex,
                           Ipv4Address apIp,
                           const Ipv4InterfaceContainer& staInterfaces)
{
    if (g_staIpsByAp.size() <= static_cast<std::size_t>(apIndex))
    {
        g_staIpsByAp.resize(apIndex + 1);
    }

    const std::string apIpString = Ipv4ToString(apIp);
    g_apByIp[apIpString] = apIndex;

    auto& staIps = g_staIpsByAp[apIndex];
    staIps.clear();
    staIps.reserve(staInterfaces.GetN());

    for (uint32_t staIndex = 0; staIndex < staInterfaces.GetN(); ++staIndex)
    {
        const std::string staIp = Ipv4ToString(staInterfaces.GetAddress(staIndex));
        staIps.push_back(staIp);
        g_staApByIp[staIp] = apIndex;
    }
}

static void
RecordMacStats(int64_t nowUs,
               const std::string& srcIp,
               const std::string& dstIp,
               uint32_t payloadBytes)
{
    if (g_trafficCoordinator->GetExperimentStartUs() < 0 ||
        nowUs < g_trafficCoordinator->GetExperimentStartUs() ||
        payloadBytes == 0)
    {
        return;
    }

    const int64_t relativeUs = nowUs - g_trafficCoordinator->GetExperimentStartUs();
    const int64_t statsEndUs = static_cast<int64_t>(std::ceil(
        g_trafficCoordinator->GetMaxExperimentDurationMs() * 1000.0));

    if (relativeUs >= statsEndUs)
    {
        return;
    }

    const uint32_t bucketIndex = static_cast<uint32_t>(
        relativeUs / kMacStatsWindowUs);

    // UL: STA -> its AP.
    auto srcSta = g_staApByIp.find(srcIp);
    auto dstAp = g_apByIp.find(dstIp);
    if (srcSta != g_staApByIp.end() &&
        dstAp != g_apByIp.end() &&
        srcSta->second == dstAp->second)
    {
        g_macWindowStats[bucketIndex][srcSta->second].upBytes[srcIp] += payloadBytes;
        return;
    }

    // DL: AP -> one of its STAs.
    auto srcAp = g_apByIp.find(srcIp);
    auto dstSta = g_staApByIp.find(dstIp);
    if (srcAp != g_apByIp.end() &&
        dstSta != g_staApByIp.end() &&
        srcAp->second == dstSta->second)
    {
        g_macWindowStats[bucketIndex][srcAp->second].downBytes[dstIp] += payloadBytes;
    }
}

static uint64_t
GetMacBytes(const MacWindowStats* stats,
            const std::string& staIp,
            bool uplink)
{
    if (!stats)
    {
        return 0;
    }

    const auto& bytesBySta = uplink ? stats->upBytes : stats->downBytes;
    auto it = bytesBySta.find(staIp);
    return it == bytesBySta.end() ? 0 : it->second;
}

static const PhyRateAccumulator*
GetPhyRateStats(const MacWindowStats* stats,
                const std::string& staIp,
                bool uplink)
{
    if (!stats)
    {
        return nullptr;
    }

    const auto& ratesBySta = uplink ? stats->upPhyRates : stats->downPhyRates;
    const auto it = ratesBySta.find(staIp);
    return it == ratesBySta.end() ? nullptr : &it->second;
}

static void
WritePhyRateJsonFields(std::ofstream& out, const PhyRateAccumulator* rateStats)
{
    out << ", \"avg_phy_data_rate_mbps\": ";
    if (rateStats && rateStats->txAttempts > 0)
    {
        out << std::fixed << std::setprecision(6) << rateStats->AverageMbps();
    }
    else
    {
        out << "null";
    }

    out << ", \"phy_tx_attempts\": "
        << (rateStats ? rateStats->txAttempts : 0)
        << ", \"phy_tx_airtime_us\": "
        << std::fixed << std::setprecision(3)
        << (rateStats ? rateStats->AirtimeUs() : 0.0);
}

struct MacSummaryStats
{
    std::map<std::string, uint64_t> upBytes;
    std::map<std::string, uint64_t> downBytes;
    std::map<std::string, PhyRateAccumulator> upPhyRates;
    std::map<std::string, PhyRateAccumulator> downPhyRates;
    uint64_t upTotalBytes{0};
    uint64_t downTotalBytes{0};
};

static uint64_t
SumMacDirectionBytes(const MacWindowStats* stats, bool uplink)
{
    if (!stats)
    {
        return 0;
    }

    const auto& bytesBySta = uplink ? stats->upBytes : stats->downBytes;
    uint64_t total = 0;

    for (const auto& [staIp, bytes] : bytesBySta)
    {
        (void)staIp;
        total += bytes;
    }

    return total;
}

static uint64_t
WriteMacFlowArray(std::ofstream& out,
                  const std::vector<std::string>& staIps,
                  const MacWindowStats* stats,
                  bool uplink,
                  MacSummaryStats& summary,
                  const std::string& indent)
{
    out << "[";

    uint64_t windowTotalBytes = 0;
    auto& summaryBytesBySta = uplink ? summary.upBytes : summary.downBytes;
    auto& summaryRatesBySta =
        uplink ? summary.upPhyRates : summary.downPhyRates;
    bool first = true;

    for (const auto& staIp : staIps)
    {
        const uint64_t bytes = GetMacBytes(stats, staIp, uplink);
        if (bytes == 0)
        {
            continue;
        }

        windowTotalBytes += bytes;
        summaryBytesBySta[staIp] += bytes;

        const PhyRateAccumulator* rateStats =
            GetPhyRateStats(stats, staIp, uplink);
        if (rateStats)
        {
            summaryRatesBySta[staIp].Merge(*rateStats);
        }

        if (uplink)
        {
            summary.upTotalBytes += bytes;
        }
        else
        {
            summary.downTotalBytes += bytes;
        }

        // Mbps for this fixed window:
        // bytes * 8 / (window_seconds * 1e6).
        // Since window_seconds = window_us / 1e6, this simplifies to
        // bytes * 8 / window_us.
        const double bwMbps =
            static_cast<double>(bytes) * 8.0 / static_cast<double>(kMacStatsWindowUs);

        if (first)
        {
            out << "\n";
            first = false;
        }
        else
        {
            out << ",\n";
        }

        out << indent
            << "{\"host_id\": \"" << staIp
            << "\", \"bytes\": " << bytes
            << ", \"bw\": " << std::fixed << std::setprecision(6) << bwMbps;
        WritePhyRateJsonFields(out, rateStats);
        out << "}";
    }

    if (!first)
    {
        out << "\n"
            << indent.substr(0, indent.size() >= 2 ? indent.size() - 2 : 0);
    }
    out << "]";
    return windowTotalBytes;
}

static void
WriteMacSummaryFlowArray(
    std::ofstream& out,
    const std::vector<std::string>& staIps,
    const std::map<std::string, uint64_t>& totalBytesBySta,
    const std::map<std::string, PhyRateAccumulator>& phyRatesBySta,
    const std::string& indent)
{
    out << "[";
    bool first = true;

    for (const auto& staIp : staIps)
    {
        auto it = totalBytesBySta.find(staIp);
        if (it == totalBytesBySta.end() || it->second == 0)
        {
            continue;
        }

        if (first)
        {
            out << "\n";
            first = false;
        }
        else
        {
            out << ",\n";
        }

        out << indent
            << "{\"host_id\": \"" << staIp
            << "\", \"total_bytes\": " << it->second;

        const auto rateIt = phyRatesBySta.find(staIp);
        WritePhyRateJsonFields(
            out,
            rateIt == phyRatesBySta.end() ? nullptr : &rateIt->second);
        out << "}";
    }

    if (!first)
    {
        out << "\n"
            << indent.substr(0, indent.size() >= 2 ? indent.size() - 2 : 0);
    }
    out << "]";
}

static std::vector<MacSummaryStats>
BuildSparseMacSummary()
{
    std::vector<MacSummaryStats> summary(g_staIpsByAp.size());

    for (const auto& [bucketIndex, apStats] : g_phyWindowStats)
    {
        (void)bucketIndex;

        for (const auto& [apId, stats] : apStats)
        {
            if (apId < 0 ||
                static_cast<std::size_t>(apId) >= summary.size())
            {
                continue;
            }

            auto& apSummary = summary[apId];

            for (const auto& [staIp, bytes] : stats.upBytes)
            {
                apSummary.upBytes[staIp] += bytes;
                apSummary.upTotalBytes += bytes;
            }

            for (const auto& [staIp, bytes] : stats.downBytes)
            {
                apSummary.downBytes[staIp] += bytes;
                apSummary.downTotalBytes += bytes;
            }
        }
    }

    return summary;
}

static bool
MacSummaryEqual(const MacSummaryStats& lhs, const MacSummaryStats& rhs)
{
    return lhs.upTotalBytes == rhs.upTotalBytes &&
           lhs.downTotalBytes == rhs.downTotalBytes &&
           lhs.upBytes == rhs.upBytes &&
           lhs.downBytes == rhs.downBytes;
}

static void
WriteMacStatsJson(const std::string& outputPath)
{
    std::ofstream out(outputPath);
    if (!out)
    {
        NS_LOG_ERROR("Cannot open PHY statistics output: " << outputPath);
        return;
    }

    const int64_t statsDurationUs = static_cast<int64_t>(std::ceil(
        g_trafficCoordinator->GetMaxExperimentDurationMs() * 1000.0));

    const uint32_t windowCount =
        statsDurationUs > 0
            ? static_cast<uint32_t>(
                  (statsDurationUs + kMacStatsWindowUs - 1) /
                  kMacStatsWindowUs)
            : 0;

    std::vector<MacSummaryStats> summaryFromWindows(g_staIpsByAp.size());
    bool windowTotalsConsistent = true;

    out << "{\n"
        << "  \"source\": \"PhyTxBegin+PhyTxPsduBegin/AppTxTag\",\n"
        << "  \"byte_semantics\": \"tagged application payload observed at PHY; retransmissions included\",\n"
        << "  \"phy_rate_semantics\": \"airtime-weighted nominal WifiTxVector data rate of actual tagged PPDU attempts; retransmissions included; PPDU airtime allocated by tagged payload bytes\",\n"
        << "  \"window_ms\": " << kMacStatsWindowMs << ",\n"
        << "  \"windows\": [\n";

    bool firstWindow = true;
    uint32_t emittedWindowCount = 0;

    // Sparse JSON output: absent windows/APs/flows mean zero traffic.
    // g_phyWindowStats itself is sparse, so do not materialize millions of
    // zero-filled 10 ms buckets in the output file.
    for (const auto& [bucketIndex, apStats] : g_phyWindowStats)
    {
        if (bucketIndex >= windowCount)
        {
            NS_LOG_ERROR("[MAC stats] bucket outside configured experiment range: bucket="
                         << bucketIndex << " windowCount=" << windowCount);
            windowTotalsConsistent = false;
            continue;
        }

        const uint32_t timestampMs = (bucketIndex + 1) * kMacStatsWindowMs;

        if (!firstWindow)
        {
            out << ",\n";
        }
        firstWindow = false;
        ++emittedWindowCount;

        out << "    {\n"
            << "      \"timestamp\": " << timestampMs << ",\n"
            << "      \"stats\": [\n";

        bool firstAp = true;

        for (const auto& [apIdInt, statsValue] : apStats)
        {
            if (apIdInt < 0 ||
                static_cast<std::size_t>(apIdInt) >= g_staIpsByAp.size())
            {
                NS_LOG_ERROR("[MAC stats] invalid AP id in bucket="
                             << bucketIndex << " AP=" << apIdInt);
                windowTotalsConsistent = false;
                continue;
            }

            const std::size_t apId = static_cast<std::size_t>(apIdInt);
            const MacWindowStats* stats = &statsValue;

            const uint64_t sparseUpTotal = SumMacDirectionBytes(stats, true);
            const uint64_t sparseDownTotal = SumMacDirectionBytes(stats, false);

            // Defensive guard: RecordMacStats only inserts positive payloads,
            // but do not serialize an empty AP object if that invariant changes.
            if (sparseUpTotal == 0 && sparseDownTotal == 0)
            {
                continue;
            }

            if (!firstAp)
            {
                out << ",\n";
            }
            firstAp = false;

            out << "        {\n"
                << "          \"ap_id\": " << apId << ",\n"
                << "          \"up_flows\": ";

            const uint64_t upTotalBytes =
                WriteMacFlowArray(out,
                                  g_staIpsByAp[apId],
                                  stats,
                                  true,
                                  summaryFromWindows[apId],
                                  "            ");

            if (upTotalBytes != sparseUpTotal)
            {
                windowTotalsConsistent = false;
                NS_LOG_ERROR("[MAC stats] UL window total mismatch: bucket="
                             << bucketIndex << " AP=" << apId
                             << " emitted=" << upTotalBytes
                             << " sparse=" << sparseUpTotal);
            }

            out << ",\n"
                << "          \"up_total_bytes\": " << upTotalBytes << ",\n"
                << "          \"down_flows\": ";

            const uint64_t downTotalBytes =
                WriteMacFlowArray(out,
                                  g_staIpsByAp[apId],
                                  stats,
                                  false,
                                  summaryFromWindows[apId],
                                  "            ");

            if (downTotalBytes != sparseDownTotal)
            {
                windowTotalsConsistent = false;
                NS_LOG_ERROR("[MAC stats] DL window total mismatch: bucket="
                             << bucketIndex << " AP=" << apId
                             << " emitted=" << downTotalBytes
                             << " sparse=" << sparseDownTotal);
            }

            out << ",\n"
                << "          \"down_total_bytes\": " << downTotalBytes
                << "\n        }";
        }

        out << "\n      ]\n"
            << "    }";
    }

    if (!firstWindow)
    {
        out << "\n";
    }

    const std::vector<MacSummaryStats> summaryFromSparse =
        BuildSparseMacSummary();

    bool summaryTotalsConsistent =
        summaryFromWindows.size() == summaryFromSparse.size();

    if (summaryTotalsConsistent)
    {
        for (std::size_t apId = 0;
             apId < summaryFromWindows.size();
             ++apId)
        {
            if (!MacSummaryEqual(summaryFromWindows[apId],
                                 summaryFromSparse[apId]))
            {
                summaryTotalsConsistent = false;
                NS_LOG_ERROR("[MAC stats] summary mismatch for AP " << apId);
            }
        }
    }

    out << "  ],\n"
        << "  \"summary\": [\n";

    for (std::size_t apId = 0; apId < summaryFromWindows.size(); ++apId)
    {
        const auto& summary = summaryFromWindows[apId];

        out << "    {\n"
            << "      \"ap_id\": " << apId << ",\n"
            << "      \"up_total_bytes\": " << summary.upTotalBytes << ",\n"
            << "      \"down_total_bytes\": " << summary.downTotalBytes << ",\n"
            << "      \"up_flows\": ";

        WriteMacSummaryFlowArray(out,
                                 g_staIpsByAp[apId],
                                 summary.upBytes,
                                 summary.upPhyRates,
                                 "        ");

        out << ",\n"
            << "      \"down_flows\": ";

        WriteMacSummaryFlowArray(out,
                                 g_staIpsByAp[apId],
                                 summary.downBytes,
                                 summary.downPhyRates,
                                 "        ");

        out << "\n"
            << "    }";

        if (apId + 1 < summaryFromWindows.size())
        {
            out << ",";
        }
        out << "\n";
    }

    out << "  ],\n"
        << "  \"validation\": {\n"
        << "    \"window_totals_consistent\": "
        << (windowTotalsConsistent ? "true" : "false") << ",\n"
        << "    \"summary_totals_consistent\": "
        << (summaryTotalsConsistent ? "true" : "false") << "\n"
        << "  }\n"
        << "}\n";

    out.close();

    NS_LOG_INFO("PHY per-node statistics written to " << outputPath
                << " (" << emittedWindowCount << " non-empty / "
                << windowCount << " total x " << kMacStatsWindowMs
                << "ms windows)"
                << ", windowTotalsConsistent="
                << (windowTotalsConsistent ? "true" : "false")
                << ", summaryTotalsConsistent="
                << (summaryTotalsConsistent ? "true" : "false"));
}

void PrintTransmissionTimePerSender()
{
    NS_LOG_INFO("========== MAC Layer Transmission time per sender ==========");

    std::map<std::string, uint64_t> senderTotalDiffUs;
    std::map<std::string, uint64_t> senderTotalBytes;

    for (const auto& [key, rxTimestamps] : g_rxMap)
    {
        auto txIt = g_txMap.find(key);
        if (txIt == g_txMap.end() || txIt->second.empty())
        {
            continue;
        }

        const auto& txTimestamps = txIt->second;
        std::size_t minSize = std::min(txTimestamps.size(), rxTimestamps.size());

        for (std::size_t i = 0; i < minSize; ++i)
        {
            int64_t diff = static_cast<int64_t>(rxTimestamps[i]) - static_cast<int64_t>(txTimestamps[i]);
            if (diff > 0)
            {
                senderTotalDiffUs[key.txSrcIp] += static_cast<uint64_t>(diff);
            }
        }
    }

    for (const auto& [k, byteArr] : g_txBytes)
    {
        int sum = std::accumulate(byteArr.begin(), byteArr.end(), 0);
        senderTotalBytes[k] = sum;
    }

    for (const auto& [sender, totalUs] : senderTotalDiffUs)
    {
        double totalMs = static_cast<double>(totalUs) / 1000.0;
        double totalSec = static_cast<double>(totalUs) / 1e6;
        double totalBytesMb = static_cast<double>(senderTotalBytes[sender]) / (1024.0 * 1024.0);
        NS_LOG_INFO("Sender " << sender <<
            ": txTime=" << totalMs << " ms (" << totalSec << " s), " <<
            "PayloadOnly=" << senderTotalBytes[sender] << " (" << totalBytesMb << " MB)" <<
            "effRate=" << totalBytesMb * 8 / totalSec << " mbps");
    }

    NS_LOG_INFO("============================================================");
}

void DeviceTxTrace (std::string context, Ptr<const Packet> packet)
{
    if (g_trafficCoordinator->GetExperimentStartUs() < 0 ||
        Simulator::Now().GetMicroSeconds() < g_trafficCoordinator->GetExperimentStartUs())
    {
        return;
    }
    Ptr<Packet> pktCopy = packet->Copy ();
    LlcSnapHeader llc;
    pktCopy->RemoveHeader (llc);

    const uint32_t packetSize = packet->GetSize ();
    if (packetSize <= 60)
    {
        return;
    }
    const uint32_t payloadSize = packetSize - 60;

    Ipv4Header ipHeader;
    std::string srcIp = "Unknown";
    std::string dstIp = "Unknown";

    if (pktCopy->PeekHeader (ipHeader))
    {
        std::ostringstream srcStream, dstStream;
        ipHeader.GetSource ().Print (srcStream);
        ipHeader.GetDestination ().Print (dstStream);
        srcIp = srcStream.str ();
        dstIp = dstStream.str ();

        pktCopy->RemoveHeader (ipHeader);
    }
    else
    {
        return;
    }

    TcpHeader tcpHeader;
    uint16_t srcPort = 0;
    uint16_t dstPort = 0;
    bool isTcp = pktCopy->PeekHeader (tcpHeader);

    if (isTcp)
    {
        srcPort = tcpHeader.GetSourcePort ();
        dstPort = tcpHeader.GetDestinationPort ();
    }
    else
    {
        return;
    }

    NS_LOG_INFO ("TX [" << Simulator::Now ().GetMicroSeconds () << " us] "
                 << "PayloadOnly: " << payloadSize << " | "
                 << "tx: " << srcIp << ":" << srcPort << " -> "
                 << "rx: " << dstIp << ":" << dstPort);

    RecordMacStats(Simulator::Now().GetMicroSeconds(),
                   srcIp,
                   dstIp,
                   payloadSize);

    TxRxKey key{srcIp, srcPort, dstIp, dstPort, payloadSize};
    g_txMap[key].push_back(Simulator::Now().GetMicroSeconds());
    g_txBytes[srcIp].push_back(payloadSize);
}

void DeviceRxTrace (std::string context, Ptr<const Packet> packet)
{
    if (g_trafficCoordinator->GetExperimentStartUs() < 0 ||
        Simulator::Now().GetMicroSeconds() < g_trafficCoordinator->GetExperimentStartUs())
    {
        return;
    }
    Ptr<Packet> pktCopy = packet->Copy ();
    LlcSnapHeader llc;
    pktCopy->RemoveHeader (llc);

    const uint32_t packetSize = packet->GetSize ();
    if (packetSize <= 60)
    {
        return;
    }
    const uint32_t payloadSize = packetSize - 60;

    Ipv4Header ipHeader;
    std::string srcIp = "Unknown";
    std::string dstIp = "Unknown";

    if (pktCopy->PeekHeader (ipHeader))
    {
        std::ostringstream srcStream, dstStream;
        ipHeader.GetSource ().Print (srcStream);
        ipHeader.GetDestination ().Print (dstStream);
        srcIp = srcStream.str ();
        dstIp = dstStream.str ();

        pktCopy->RemoveHeader (ipHeader);
    }
    else
    {
        return;
    }

    TcpHeader tcpHeader;
    uint16_t srcPort = 0;
    uint16_t dstPort = 0;
    bool isTcp = pktCopy->PeekHeader (tcpHeader);

    if (isTcp)
    {
        srcPort = tcpHeader.GetSourcePort ();
        dstPort = tcpHeader.GetDestinationPort ();
    }
    else
    {
        return;
    }

    NS_LOG_INFO ("RX [" << Simulator::Now ().GetMicroSeconds() << " us] "
                 << "Payload: " << payloadSize << " | "
                 << "tx: " << srcIp << ":" << srcPort << " -> "
                 << "rx: " << dstIp << ":" << dstPort);
    TxRxKey key{srcIp, srcPort, dstIp, dstPort, payloadSize};
    g_rxMap[key].push_back(Simulator::Now().GetMicroSeconds());
}

// ============================================================================
// Wi-Fi association diagnostics
// ============================================================================

static void
StaAssociated(int apIndex, uint32_t staIndex, Mac48Address bssid)
{
    NS_LOG_INFO("[Wi-Fi association] AP group " << apIndex
                << " STA " << staIndex
                << " associated with BSSID " << bssid);
}

// ============================================================================
// Helper: set up one AP + its stations
// ============================================================================

static void
SetupApGroup(int apIndex,
             int bandwidthMhz,
             const std::map<std::string, Address>& agentStationMap,
             const AgentMap& agentMap,
             Address apAddress,
             uint32_t staNum,
             TrafficCoordinator& trafficCoordinator)
{
    NS_LOG_INFO("=== Setting up AP group " << apIndex
                << ", BW " << bandwidthMhz << " MHz ===");

    // Create nodes
    NodeContainer apNode;
    apNode.Create(1); // AP node

    NodeContainer stationNodes;
    stationNodes.Create(staNum); // stations assigned to this AP

    // Create a physically isolated YansWifiChannel for this AP group.
    // PHYs belonging to different AP groups cannot hear or interfere with
    // one another, regardless of their coordinates or configured frequency.
    YansWifiChannelHelper channelHelper = YansWifiChannelHelper::Default();
    Ptr<YansWifiChannel> channel = channelHelper.Create();

    // The channel number intentionally remains 0: this hard model represents
    // physical separation through independent channel objects rather than
    // through per-AP RF channel numbers such as 36, 100, and 149.

    // Configure PHY
    YansWifiPhyHelper phyHelper;
    phyHelper.SetChannel(channel);
    phyHelper.Set("ChannelSettings",
                  StringValue("{0, " + std::to_string(bandwidthMhz) +
                              ", BAND_5GHZ, 0}"));

    // Configure MAC (802.11ax)
    WifiHelper wifiHelper;
    wifiHelper.SetStandard(WIFI_STANDARD_80211ax);
    wifiHelper.SetRemoteStationManager("ns3::MinstrelHtWifiManager");

    // Each hard-assigned AP group has a unique SSID. Stations actively probe
    // for that SSID and associate with the AP in their isolated radio domain.
    const std::string ssidName = "llm-ap-" + std::to_string(apIndex);
    const Ssid ssid(ssidName);

    WifiMacHelper macHelper;
    macHelper.SetType("ns3::ApWifiMac",
                      "Ssid", SsidValue(ssid));

    WifiMacHelper staMacHelper;
    staMacHelper.SetType("ns3::StaWifiMac",
                         "Ssid", SsidValue(ssid),
                         "ActiveProbing", BooleanValue(true));

    // Install devices on AP
    NetDeviceContainer apDevices =
        wifiHelper.Install(phyHelper, macHelper, apNode);

    // Install devices on stations
    NetDeviceContainer staDevices =
        wifiHelper.Install(phyHelper, staMacHelper, stationNodes);

    // Log successful Wi-Fi associations. These traces distinguish actual
    // 802.11 association from a later TCP connection callback.
    for (uint32_t i = 0; i < staDevices.GetN(); ++i)
    {
        Ptr<WifiNetDevice> wifiDevice = DynamicCast<WifiNetDevice>(staDevices.Get(i));
        NS_ABORT_MSG_IF(!wifiDevice,
                        "STA device " << i << " in AP group " << apIndex
                                      << " is not a WifiNetDevice");

        Ptr<StaWifiMac> staMac = DynamicCast<StaWifiMac>(wifiDevice->GetMac());
        NS_ABORT_MSG_IF(!staMac,
                        "STA device " << i << " in AP group " << apIndex
                                      << " does not use StaWifiMac");

        staMac->TraceConnectWithoutContext(
            "Assoc",
            MakeBoundCallback(&StaAssociated, apIndex, i));
    }

    // Mobility: fixed positions
    MobilityHelper mobility;

    // AP at center
    mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    mobility.Install(apNode);

    // Set AP position to center
    Ptr<ConstantPositionMobilityModel> apMob =
        apNode.Get(0)->GetObject<ConstantPositionMobilityModel>();
    apMob->SetPosition(Vector(100 * apIndex, 100 * apIndex, 100 * apIndex));

    // Stations in a disc around AP (5m radius)
    // mobility.SetPositionAllocator("ns3::RandomDiscPositionAllocator",
    //                               "Center", VectorValue(Vector(100 * apIndex, 100 * apIndex, 100 * apIndex)),
    //                               "MaxDistance", DoubleValue(5.0));
    mobility.SetPositionAllocator("ns3::UniformDiscPositionAllocator",
                                  "X", DoubleValue(100 * apIndex),
                                  "Y", DoubleValue(100 * apIndex),
                                  "Z", DoubleValue(100 * apIndex),
                                  "rho", DoubleValue(5.0));
    mobility.Install(stationNodes);

    // Internet stack
    InternetStackHelper stack;
    stack.Install(apNode);
    stack.Install(stationNodes);

    // Assign AP first, then stations. The AP receives 10.1.<apIndex>.1.
    Ipv4AddressHelper ipv4;
    std::string subnet = "10.1." + std::to_string(apIndex) + ".0";
    ipv4.SetBase(Ipv4Address(subnet.c_str()), Ipv4Mask("255.255.255.0"));

    NS_LOG_INFO("Number of devices in apDevices: " << apDevices.GetN());
    Ipv4InterfaceContainer apInterfaces = ipv4.Assign(apDevices);
    Ipv4InterfaceContainer staInterfaces = ipv4.Assign(staDevices);

    RegisterApGroupForMacStats(apIndex,
                               apInterfaces.GetAddress(0),
                               staInterfaces);

    RegisterWifiObservability(
        apNode.Get(0)->GetId(),
        "AP" + std::to_string(apIndex) + "(" +
            Ipv4ToString(apInterfaces.GetAddress(0)) + ")",
        apDevices.Get(0));

    for (uint32_t i = 0; i < staDevices.GetN(); ++i)
    {
        RegisterWifiObservability(
            stationNodes.Get(i)->GetId(),
            "AP" + std::to_string(apIndex) + "/STA" + std::to_string(i) + "(" +
                Ipv4ToString(staInterfaces.GetAddress(i)) + ")",
            staDevices.Get(i));
    }

    NS_LOG_INFO("AP location X:" << 100 * apIndex
                << " Y:" << 100 * apIndex
                << " Z:" << 100 * apIndex
                << ", stations randomly within 5m distance");
    NS_LOG_INFO("AP SSID: " << ssidName);
    NS_LOG_INFO("AP IP: " << apInterfaces.GetAddress(0));
    NS_LOG_INFO("STA N: " << staInterfaces.GetN());
    NS_LOG_INFO("STA IP1: " << staInterfaces.GetAddress(0));
    NS_LOG_INFO("STA IP2: " << staInterfaces.GetAddress(1));
    NS_LOG_INFO("STA IP3: " << staInterfaces.GetAddress(2));

    // ========================================================================
    // Install TrafficSink on AP (receives uplink from stations)
    // ========================================================================
    Ptr<TrafficSink> apSink = CreateObject<TrafficSink>();
    apSink->SetAttribute("Port", UintegerValue(10000));
    apNode.Get(0)->AddApplication(apSink);
    apSink->SetStartTime(Seconds(0));
    trafficCoordinator.AddApplication(apSink);

    // ========================================================================
    // Install TrafficSink on each station (receives downlink from AP)
    // ========================================================================
    for (uint32_t i = 0; i < stationNodes.GetN(); i++)
    {
        Ptr<TrafficSink> sink = CreateObject<TrafficSink>();
        sink->SetAttribute("Port", UintegerValue(9000 + i));
        stationNodes.Get(i)->AddApplication(sink);
        sink->SetStartTime(Seconds(0));
        trafficCoordinator.AddApplication(sink);
    }

    // Starting a generator only starts TCP setup. Payload scheduling is held
    // behind the global barrier until every generator reports ready.
    const Time trafficGeneratorStart = Seconds(1.0);

    // ========================================================================
    // Install StaLlmGenerator on stations (uplink to AP)
    // One StaLlmGenerator per station, handling ALL agents on that station
    // Uses single TCP socket per station shared by all agents
    // ========================================================================

    // Group agents by station index
    std::map<int, std::vector<std::pair<std::string, std::vector<Operation>>>> stationToAgents;
    for (const auto& [agentKey, stationAddr] : agentStationMap)
    {
        auto addr = InetSocketAddress::ConvertFrom(stationAddr);
        Ipv4Address ip = addr.GetIpv4();
        uint32_t ipv4 = ip.Get();
        uint32_t lastOctet = ipv4 & 0xff;
        int stationIndex = static_cast<int>(lastOctet - 2);

        NS_ABORT_MSG_IF(stationIndex < 0 ||
                            stationIndex >= static_cast<int>(stationNodes.GetN()),
                        "Invalid station index " << stationIndex
                                                 << " for agent " << agentKey
                                                 << " in AP group " << apIndex);

        stationToAgents[stationIndex].push_back({agentKey, agentMap.at(agentKey)});
    }

    // Create one StaLlmGenerator per station that has agents
    for (auto& [idx, agents] : stationToAgents)
    {
        Ptr<StaLlmGenerator> gen = CreateObject<StaLlmGenerator>();
        gen->SetAttribute("Remote", AddressValue(apAddress));
        gen->SetReadyCallback(trafficCoordinator.GetReadyCallback());

        // Merge all operations from all agents on this station
        std::map<std::string, std::vector<std::tuple<int, double, double, int>>> agentOpsMap;
        for (const auto& [aKey, ops] : agents)
        {
            for (const auto& op : ops)
            {
                agentOpsMap[aKey].push_back(
                    std::make_tuple(op.downlinkBytes,
                                    op.endMs,
                                    op.startOffsetMs,
                                    op.uplinkBytes));
            }
        }
        gen->SetAgentMap(agentOpsMap);

        const uint32_t staNodeId = stationNodes.Get(idx)->GetId();
        gen->TraceConnectWithoutContext(
            "TxCustom",
            MakeBoundCallback(&StaAppTxTrace, staNodeId));
        gen->TraceConnectWithoutContext(
            "AppTxDrop",
            MakeBoundCallback(&StaAppTxDropTrace, staNodeId));

        stationNodes.Get(idx)->AddApplication(gen);
        gen->SetStartTime(trafficGeneratorStart);

        trafficCoordinator.AddGenerator(gen);
        trafficCoordinator.AddApplication(gen);

        NS_LOG_INFO("Station " << idx << " placed on node "
                    << idx << " with " << agents.size() << " agents");
    }

    // ========================================================================
    // Install APGenerator on AP (downlink to stations)
    // Uses agentStationMap directly with string keys
    // ========================================================================
    Ptr<APGenerator> apGen = CreateObject<APGenerator>();
    apGen->SetReadyCallback(trafficCoordinator.GetReadyCallback());

    // Pass operations directly - tuple format: (downlinkBytes, endMs, startOffsetMs, uplinkBytes)
    std::map<std::string, std::vector<std::tuple<int, double, double, int>>> rawOpsMap;
    for (const auto& [agentKey, ops] : agentMap)
    {
        for (const auto& op : ops)
        {
            rawOpsMap[agentKey].push_back(
                std::make_tuple(op.downlinkBytes,
                                op.endMs,
                                op.startOffsetMs,
                                op.uplinkBytes));
        }
    }
    // Pass station map directly (already uses string keys from agent-distribution)
    apGen->SetAgentStationMap(agentStationMap);
    apGen->SetAgentMap(rawOpsMap);

    const uint32_t apNodeId = apNode.Get(0)->GetId();
    apGen->TraceConnectWithoutContext(
        "Tx",
        MakeBoundCallback(&ApAppTxTrace, apNodeId));
    apGen->TraceConnectWithoutContext(
        "AppTxDrop",
        MakeBoundCallback(&ApAppTxDropTrace, apNodeId));

    apNode.Get(0)->AddApplication(apGen);
    apGen->SetStartTime(trafficGeneratorStart);

    trafficCoordinator.AddGenerator(apGen);
    trafficCoordinator.AddApplication(apGen);

    NS_LOG_INFO("AP group " << apIndex << " setup complete: "
                << agentMap.size() << " agents, " << staNum << " stations");
}

// ============================================================================
//
// ============================================================================

int
main(int argc, char* argv[])
{
    RngSeedManager::SetSeed(12345);
    RngSeedManager::SetRun(1);

    // TcpCubic
    // TcpHighSpeed
    // TcpBbr
    // TcpLinuxReno
    Config::SetDefault("ns3::TcpL4Protocol::SocketType", TypeIdValue(TcpHighSpeed::GetTypeId()));

    // Parse command line
    std::string jsonPath;
    std::string statsOutputPath = "mac-node-stats.json";
    std::string experimentTimeArg = "auto";
    bool autoExperimentTime = true;
    double fixedExperimentTimeMs = 0.0;
    int bandwidthMhz = 20;
    int apNum = 3;
    int staNum = 30;

    if (argc < 2)
    {
        std::cerr << "Usage: " << argv[0]
                  << " <traces.json> [bandwidth_mhz] [stats_output.json] [experiment_time]"
                  << "\n  bandwidth_mhz: 20, 40, 80 or 160 (default: 20)"
                  << "\n  stats_output.json: default mac-node-stats.json"
                  << "\n  experiment_time: auto (JSON duration + 2s, default) or fixed seconds > 0"
                  << std::endl;
        return 1;
    }

    jsonPath = argv[1];
    if (argc >= 3)
    {
        bandwidthMhz = std::stoi(argv[2]);
    }
    if (argc >= 4)
    {
        statsOutputPath = argv[3];
    }
    if (argc >= 5)
    {
        experimentTimeArg = argv[4];

        if (experimentTimeArg != "auto")
        {
            try
            {
                std::size_t parsedChars = 0;
                const double fixedExperimentTimeSeconds =
                    std::stod(experimentTimeArg, &parsedChars);

                if (parsedChars != experimentTimeArg.size() ||
                    !std::isfinite(fixedExperimentTimeSeconds) ||
                    fixedExperimentTimeSeconds <= 0.0)
                {
                    throw std::invalid_argument("invalid experiment time");
                }

                autoExperimentTime = false;
                fixedExperimentTimeMs = fixedExperimentTimeSeconds * 1000.0;
            }
            catch (const std::exception&)
            {
                std::cerr << "Invalid experiment_time: " << experimentTimeArg
                          << ". Expected 'auto' or a positive number of seconds."
                          << std::endl;
                return 1;
            }
        }
    }
    if (argc > 5)
    {
        std::cerr << "Too many command-line arguments." << std::endl;
        return 1;
    }

    if (bandwidthMhz != 20 &&
        bandwidthMhz != 40 &&
        bandwidthMhz != 80 &&
        bandwidthMhz != 160)
    {
        std::cerr << "Unsupported bandwidth: " << bandwidthMhz
                  << " MHz. Expected 20, 40, 80 or 160." << std::endl;
        return 1;
    }

    std::cout << "=== ns-3 Sample Scenario: " << apNum << " APs x "
              << staNum << " Stations ===" << std::endl;
    std::cout << "JSON: " << jsonPath << std::endl;
    std::cout << "Bandwidth: " << bandwidthMhz << " MHz" << std::endl;
    std::cout << "MAC stats JSON: " << statsOutputPath << std::endl;
    std::cout << "Standard: 802.11ax (Wi-Fi 6)" << std::endl;
    std::cout << "Transport: TCP" << std::endl;
    std::cout << "Channel model: separate YansWifiChannel per AP group" << std::endl;
    std::cout << "Channel policy: physically isolated AP groups; default 5 GHz channel"
              << std::endl;

    // Enable logging
    LogComponentEnable("SampleScenario", LOG_LEVEL_INFO);
    LogComponentEnable("APGenerator", LOG_LEVEL_WARN);
    LogComponentEnable("StaLlmGenerator", LOG_LEVEL_WARN);
    LogComponentEnable("TrafficSink", LOG_LEVEL_WARN);
    // LogComponentEnable("AgentDistribution", LOG_LEVEL_INFO);
    LogComponentEnable("ContentionAwareAgentDistribution", LOG_LEVEL_INFO);

    // Parse and distribute agents
    ParsedResult parsed = ParseJsonFile(jsonPath);
    const double traceDurationMs = parsed.experimentDurationMs;
    const double maxExperimentDurationMs =
        autoExperimentTime ? traceDurationMs + kAutoExperimentTailMarginMs
                           : fixedExperimentTimeMs;
    TrafficCoordinator trafficCoordinator(traceDurationMs, maxExperimentDurationMs);
    g_trafficCoordinator = &trafficCoordinator;

    // Default distribution
    // DistributionResult dist = DistributeAgents(parsed, apNum, staNum, 3);

    // Distribution with contention awareness
    ContentionAwareDistributionConfig distributionConfig;

    distributionConfig.nAp = apNum;
    distributionConfig.nStationsPerAp = staNum;
    // 0 = unlimited application-level agents per physical STA.
    distributionConfig.maxAgentsPerStation = 832;

    // true:
    //   contention важнее количества используемых STA.
    //
    // false:
    //   сначала стараемся задействовать максимально возможное число STA.
    distributionConfig.lowContentionPriority = true;

    // Размер окна для приблизительного определения одновременного UL.
    // Можно свободно менять для экспериментов.
    distributionConfig.slotMs = 10;

    DistributionResult dist =
        DistributeAgentsContentionAware(
            parsed,
            distributionConfig);

    // Set up each AP group
    for (int ap = 0; ap < apNum; ap++)
    {
        SetupApGroup(ap,
                     bandwidthMhz,
                     dist.apStationMaps[ap],
                     dist.apAgentMaps[ap],
                     dist.apAddresses[ap],
                     staNum,
                     trafficCoordinator);
    }

    trafficCoordinator.FinalizeRegistration();

    if (autoExperimentTime)
    {
        std::cout << "\nExperiment time mode: auto" << std::endl;
        std::cout << "Experiment duration from JSON: "
                  << g_trafficCoordinator->GetTraceDurationMs() / 1000.0 << " seconds" << std::endl;
        std::cout << "Max experiment time: "
                  << g_trafficCoordinator->GetMaxExperimentDurationMs() / 1000.0
                  << " seconds (JSON duration + "
                  << kAutoExperimentTailMarginMs / 1000.0
                  << " seconds)" << std::endl;
    }
    else
    {
        std::cout << "\nExperiment time mode: fixed" << std::endl;
        std::cout << "Max experiment time: "
                  << g_trafficCoordinator->GetMaxExperimentDurationMs() / 1000.0
                  << " seconds" << std::endl;
        std::cout << "Experiment duration from JSON: "
                  << g_trafficCoordinator->GetTraceDurationMs() / 1000.0 << " seconds" << std::endl;

        if (g_trafficCoordinator->GetMaxExperimentDurationMs() < g_trafficCoordinator->GetTraceDurationMs())
        {
            NS_LOG_WARN("Fixed experiment time "
                        << g_trafficCoordinator->GetMaxExperimentDurationMs() / 1000.0
                        << "s is shorter than JSON duration "
                        << g_trafficCoordinator->GetTraceDurationMs() / 1000.0
                        << "s; trace tail will be truncated");
        }
    }
    std::cout << "Waiting for " << trafficCoordinator.GetExpectedGeneratorCount()
              << " traffic generators to complete TCP setup..." << std::endl;

    Config::Connect ("/NodeList/*/DeviceList/*/$ns3::WifiNetDevice/Mac/MacTx",
                 MakeCallback (&DeviceTxTrace));

    Config::Connect ("/NodeList/*/DeviceList/*/$ns3::WifiNetDevice/Mac/MacRx",
                 MakeCallback (&DeviceRxTrace));

    Config::SetDefault(
    "ns3::TcpL4Protocol::SocketType",
    TypeIdValue(TcpLinuxReno::GetTypeId()));
    Config::SetDefault("ns3::TcpSocket::SegmentSize", UintegerValue(1460));
    Config::SetDefault("ns3::TcpSocket::SndBufSize", UintegerValue(32 * 1024 * 1024));
    Config::SetDefault("ns3::TcpSocket::RcvBufSize", UintegerValue(32 * 1024 * 1024));

    const auto wallClockStart = std::chrono::steady_clock::now();

    Simulator::Run();

    const auto wallClockEnd = std::chrono::steady_clock::now();
    const double wallClockSeconds =
        std::chrono::duration<double>(wallClockEnd - wallClockStart).count();

    NS_ABORT_MSG_IF(g_trafficCoordinator->GetExperimentStartUs() < 0,
                    "Simulation ended before the global traffic barrier opened");

    WriteMacStatsJson(statsOutputPath);
    PrintTransmissionTimePerSender();
    PrintCrossLayerStats();

    Simulator::Destroy();

    std::cout << "\n=== Simulation Complete ===" << std::endl;
    std::cout << "Total events: " << Simulator::GetEventCount() << std::endl;
    NS_LOG_INFO("[Realtime] Simulator::Run wall-clock time: "
                << std::fixed << std::setprecision(3)
                << wallClockSeconds << " seconds");
    std::cout << "Realtime simulation runtime: "
              << std::fixed << std::setprecision(3)
              << wallClockSeconds << " seconds" << std::endl;

    return 0;
}
