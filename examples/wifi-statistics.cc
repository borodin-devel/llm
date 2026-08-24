#include "wifi-statistics.h"

#include "wifi-statistics-internal.h"

#include "scenario-log.h"
#include "traffic-coordinator.h"

#include "ns3/ap-generator.h"
#include "ns3/app-tx-tag.h"
#include "ns3/core-module.h"
#include "ns3/internet-module.h"
#include "ns3/network-module.h"
#include "ns3/sta-llm-generator.h"
#include "ns3/wifi-module.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace ns3
{

static LogComponent& g_log = llm_example::GetScenarioLog();
static constexpr uint32_t kMacStatsWindowMs = 10;
static constexpr int64_t kMacStatsWindowUs =
    static_cast<int64_t>(kMacStatsWindowMs) * 1000;

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

static bool
GetExperimentSecond(int64_t absoluteUs, uint32_t& second)
{
    if (GetWifiStatisticsState().coordinator.GetExperimentStartUs() < 0 || absoluteUs < GetWifiStatisticsState().coordinator.GetExperimentStartUs())
    {
        return false;
    }

    const int64_t relativeUs = absoluteUs - GetWifiStatisticsState().coordinator.GetExperimentStartUs();
    const int64_t experimentEndUs = static_cast<int64_t>(std::ceil(
        GetWifiStatisticsState().coordinator.GetMaxExperimentDurationMs() * 1000.0));
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
    return &GetWifiStatisticsState().nodeSeconds[nodeId][second];
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
    const auto srcSta = GetWifiStatisticsState().bssByStationIp.find(srcIp);
    const auto dstAp = GetWifiStatisticsState().bssByApIp.find(dstIp);
    if (srcSta != GetWifiStatisticsState().bssByStationIp.end() &&
        dstAp != GetWifiStatisticsState().bssByApIp.end() &&
        srcSta->second == dstAp->second)
    {
        apId = srcSta->second;
        hostId = srcIp;
        uplink = true;
        return true;
    }

    const auto srcAp = GetWifiStatisticsState().bssByApIp.find(srcIp);
    const auto dstSta = GetWifiStatisticsState().bssByStationIp.find(dstIp);
    if (srcAp != GetWifiStatisticsState().bssByApIp.end() &&
        dstSta != GetWifiStatisticsState().bssByStationIp.end() &&
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
    if (GetWifiStatisticsState().coordinator.GetExperimentStartUs() < 0 || nowUs < GetWifiStatisticsState().coordinator.GetExperimentStartUs())
    {
        return false;
    }

    const int64_t relativeUs = nowUs - GetWifiStatisticsState().coordinator.GetExperimentStartUs();
    const int64_t statsEndUs = static_cast<int64_t>(std::ceil(
        GetWifiStatisticsState().coordinator.GetMaxExperimentDurationMs() * 1000.0));
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

    auto& stats = GetWifiStatisticsState().phyWindows[bucketIndex][apId];
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
        firstTransmission = GetWifiStatisticsState().seenTaggedMpdus.insert(key).second;
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

        auto& stats = GetWifiStatisticsState().phyWindows[bucketIndex][apId];
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

    if (GetWifiStatisticsState().coordinator.GetExperimentStartUs() < 0)
    {
        return;
    }

    int64_t intervalStartUs = std::max<int64_t>(
        start.GetMicroSeconds(),
        GetWifiStatisticsState().coordinator.GetExperimentStartUs());
    int64_t intervalEndUs = start.GetMicroSeconds() + duration.GetMicroSeconds();
    const int64_t experimentEndUs = GetWifiStatisticsState().coordinator.GetExperimentStartUs() +
        static_cast<int64_t>(std::ceil(GetWifiStatisticsState().coordinator.GetMaxExperimentDurationMs() * 1000.0));
    intervalEndUs = std::min(intervalEndUs, experimentEndUs);

    while (intervalStartUs < intervalEndUs)
    {
        const int64_t relativeUs = intervalStartUs - GetWifiStatisticsState().coordinator.GetExperimentStartUs();
        const uint32_t second = static_cast<uint32_t>(relativeUs / 1000000);
        const int64_t nextBoundaryUs =
            GetWifiStatisticsState().coordinator.GetExperimentStartUs() + (static_cast<int64_t>(second) + 1) * 1000000;
        const int64_t pieceEndUs = std::min(intervalEndUs, nextBoundaryUs);
        GetWifiStatisticsState().nodeSeconds[nodeId][second].phyBusyUs += pieceEndUs - intervalStartUs;
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

    GetWifiStatisticsState().nodeLabels[nodeId] = label;

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

void
WifiStatistics::RegisterWifiDevice(uint32_t nodeId,
                                   std::string nodeLabel,
                                   Ptr<NetDevice> device)
{
    RegisterWifiObservability(nodeId, nodeLabel, device);
}

void
WifiStatistics::ConnectApGenerator(Ptr<APGenerator> generator, uint32_t nodeId)
{
    generator->TraceConnectWithoutContext("Tx", MakeBoundCallback(&ApAppTxTrace, nodeId));
    generator->TraceConnectWithoutContext("AppTxDrop",
                                          MakeBoundCallback(&ApAppTxDropTrace, nodeId));
}

void
WifiStatistics::ConnectStaGenerator(Ptr<StaLlmGenerator> generator, uint32_t nodeId)
{
    generator->TraceConnectWithoutContext("TxCustom",
                                          MakeBoundCallback(&StaAppTxTrace, nodeId));
    generator->TraceConnectWithoutContext("AppTxDrop",
                                          MakeBoundCallback(&StaAppTxDropTrace, nodeId));
}

} // namespace ns3
