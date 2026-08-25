#include "traffic-coordinator.h"
#include "wifi-statistics-internal.h"

#include "ns3/app-tx-tag.h"
#include "ns3/simulator.h"
#include "ns3/wifi-module.h"

#include <cmath>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace ns3
{

namespace
{

/** Exact endpoint and direction resolution for one tagged BSS flow. */
struct PhyFlowAttribution
{
    const ExperimentEntityIdentity* sender;   ///< Registered transmitting entity.
    const ExperimentEntityIdentity* receiver; ///< Registered traffic peer.
    ExperimentDirection direction;            ///< Direction shared by both entity views.
};

/** Transitional endpoint resolution for one tagged BSS flow. */
struct LegacyPhyFlowAttribution
{
    int accessPointId;       ///< Legacy BSS index.
    std::string stationIpv4; ///< Legacy station map key.
    bool uplink;             ///< Whether the flow is uplink.
};

/** Tagged flow contribution extracted from one PPDU. */
struct PpduFlowContribution
{
    uint64_t taggedBytes{0};            ///< Tagged bytes assigned to this flow.
    long double rateBpsTimesBytes{0.0}; ///< PHY rate multiplied by tagged bytes.
};

std::string
Ipv4ToString(Ipv4Address address)
{
    std::ostringstream stream;
    address.Print(stream);
    return stream.str();
}

std::string
MacToString(Mac48Address address)
{
    std::ostringstream stream;
    stream << address;
    return stream.str();
}

int64_t
GetExperimentDurationUs(const WifiStatisticsState& statistics)
{
    return ConvertExperimentDurationMsToUs(statistics.coordinator.GetMaxExperimentDurationMs());
}

std::optional<PhyFlowAttribution>
ResolvePhyFlow(const WifiStatisticsState& statistics,
               const std::string& sourceIpv4,
               const std::string& destinationIpv4)
{
    const auto* source = statistics.entityRegistry.FindByIpv4(sourceIpv4);
    const auto* destination = statistics.entityRegistry.FindByIpv4(destinationIpv4);
    if (!source || !destination || source->accessPointId != destination->accessPointId)
    {
        return std::nullopt;
    }

    if (source->kind == ExperimentEntityKind::STATION &&
        destination->kind == ExperimentEntityKind::ACCESS_POINT)
    {
        return PhyFlowAttribution{source, destination, ExperimentDirection::UPLINK};
    }
    if (source->kind == ExperimentEntityKind::ACCESS_POINT &&
        destination->kind == ExperimentEntityKind::STATION)
    {
        return PhyFlowAttribution{source, destination, ExperimentDirection::DOWNLINK};
    }
    return std::nullopt;
}

std::optional<LegacyPhyFlowAttribution>
ResolveLegacyPhyFlow(const WifiStatisticsState& statistics,
                     const std::string& sourceIpv4,
                     const std::string& destinationIpv4)
{
    const auto sourceStation = statistics.bssByStationIp.find(sourceIpv4);
    const auto destinationAp = statistics.bssByApIp.find(destinationIpv4);
    if (sourceStation != statistics.bssByStationIp.end() &&
        destinationAp != statistics.bssByApIp.end() &&
        sourceStation->second == destinationAp->second)
    {
        return LegacyPhyFlowAttribution{sourceStation->second, sourceIpv4, true};
    }

    const auto sourceAp = statistics.bssByApIp.find(sourceIpv4);
    const auto destinationStation = statistics.bssByStationIp.find(destinationIpv4);
    if (sourceAp != statistics.bssByApIp.end() &&
        destinationStation != statistics.bssByStationIp.end() &&
        sourceAp->second == destinationStation->second)
    {
        return LegacyPhyFlowAttribution{sourceAp->second, destinationIpv4, false};
    }
    return std::nullopt;
}

PhyDirectionAccumulator&
GetPhyDirection(LocalEntityWindowAccumulator& local, ExperimentDirection direction)
{
    return direction == ExperimentDirection::UPLINK ? local.phy.uplink : local.phy.downlink;
}

void
AddPayload(PhyDirectionAccumulator& accumulator,
           uint32_t peerNodeId,
           uint32_t bytes,
           bool firstTransmission)
{
    accumulator.taggedPayloadBytes += bytes;
    auto& peer = accumulator.peersByNodeId[peerNodeId];
    peer.taggedPayloadBytes += bytes;
    if (firstTransmission)
    {
        accumulator.uniqueTaggedPayloadBytes += bytes;
        peer.uniqueTaggedPayloadBytes += bytes;
    }
}

void
AddRate(PhyDirectionAccumulator& accumulator,
        uint32_t peerNodeId,
        double dataRateBps,
        long double allocatedAirtimeUs)
{
    ++accumulator.transmissionAttemptCount;
    accumulator.dataRateBpsUs += static_cast<long double>(dataRateBps) * allocatedAirtimeUs;
    accumulator.transmissionAirtimeUs += allocatedAirtimeUs;

    auto& peer = accumulator.peersByNodeId[peerNodeId];
    ++peer.transmissionAttemptCount;
    peer.dataRateBpsUs += static_cast<long double>(dataRateBps) * allocatedAirtimeUs;
    peer.transmissionAirtimeUs += allocatedAirtimeUs;
}

NodeSecondStats*
GetLegacyNodeSecond(WifiStatisticsState& statistics, uint32_t nodeId, int64_t absoluteTimeUs)
{
    const int64_t experimentStartUs = statistics.coordinator.GetExperimentStartUs();
    if (experimentStartUs < 0)
    {
        return nullptr;
    }
    uint64_t secondIndex = 0;
    if (!GetNodeSecondIndex(absoluteTimeUs - experimentStartUs,
                            GetExperimentDurationUs(statistics),
                            secondIndex))
    {
        return nullptr;
    }
    return &statistics.nodeSeconds[nodeId][secondIndex];
}

void
RecordLegacyPhyPayload(WifiStatisticsState& statistics,
                       uint64_t windowIndex,
                       const LegacyPhyFlowAttribution& flow,
                       uint32_t bytes)
{
    auto& legacy = statistics.phyWindows[windowIndex][flow.accessPointId];
    if (flow.uplink)
    {
        legacy.upBytes[flow.stationIpv4] += bytes;
    }
    else
    {
        legacy.downBytes[flow.stationIpv4] += bytes;
    }
}

void
RecordLegacyPhyRate(WifiStatisticsState& statistics,
                    uint64_t windowIndex,
                    const LegacyPhyFlowAttribution& flow,
                    double dataRateBps,
                    long double allocatedAirtimeUs)
{
    auto& legacy = statistics.phyWindows[windowIndex][flow.accessPointId];
    auto& rates = flow.uplink ? legacy.upPhyRates : legacy.downPhyRates;
    rates[flow.stationIpv4].Add(dataRateBps, static_cast<double>(allocatedAirtimeUs));
}

void
PhyTxBeginTrace(WifiStatisticsState* statistics,
                uint32_t nodeId,
                Ptr<const Packet> packet,
                double txPowerW)
{
    (void)txPowerW;
    RecordPhyTxBeginPacket(*statistics, nodeId, Simulator::Now().GetMicroSeconds(), packet);
}

void
PhyTxPsduBeginTrace(WifiStatisticsState* statistics,
                    uint32_t nodeId,
                    WifiPhyBand band,
                    WifiConstPsduMap psduMap,
                    WifiTxVector txVector,
                    double txPowerW)
{
    (void)txPowerW;
    RecordPhyTxPsduBegin(*statistics,
                         nodeId,
                         Simulator::Now().GetMicroSeconds(),
                         band,
                         psduMap,
                         txVector);
}

void
PhyStateTrace(WifiStatisticsState* statistics,
              uint32_t nodeId,
              Time start,
              Time duration,
              WifiPhyState state)
{
    if (state == WifiPhyState::TX || state == WifiPhyState::RX || state == WifiPhyState::CCA_BUSY)
    {
        RecordPhyBusyInterval(*statistics,
                              nodeId,
                              start.GetMicroSeconds(),
                              duration.GetMicroSeconds());
    }
}

} // namespace

void
RecordPhyTxBeginPacket(WifiStatisticsState& statistics,
                       uint32_t transmitterNodeId,
                       int64_t absoluteTimeUs,
                       Ptr<const Packet> packet)
{
    std::vector<PhyTaggedPayloadSpan> spans;
    std::optional<uint64_t> firstApplicationPacketUid;
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
        if (taggedBytes == 0)
        {
            continue;
        }
        if (!firstApplicationPacketUid)
        {
            firstApplicationPacketUid = tag.GetAppPacketUid();
        }
        spans.push_back({Ipv4ToString(tag.GetSource()),
                         Ipv4ToString(tag.GetDestination()),
                         tag.GetAppTxTimeUs(),
                         taggedBytes});
    }

    if (spans.empty())
    {
        return;
    }

    std::optional<PhyMpduKey> identity;
    WifiMacHeader wifiHeader;
    if (packet->PeekHeader(wifiHeader) > 0 && wifiHeader.IsData())
    {
        identity.emplace(transmitterNodeId,
                         MacToString(wifiHeader.GetAddr1()),
                         MacToString(wifiHeader.GetAddr2()),
                         wifiHeader.GetSequenceNumber(),
                         wifiHeader.GetFragmentNumber(),
                         *firstApplicationPacketUid);
    }
    RecordPhyMpduAttempt(statistics,
                         transmitterNodeId,
                         absoluteTimeUs,
                         packet->GetSize(),
                         spans,
                         identity);
}

void
RecordPhyMpduAttempt(WifiStatisticsState& statistics,
                     uint32_t transmitterNodeId,
                     int64_t absoluteTimeUs,
                     uint32_t completeMpduBytes,
                     const std::vector<PhyTaggedPayloadSpan>& spans,
                     const std::optional<PhyMpduKey>& identity)
{
    ExperimentWindowBounds bounds;
    if (spans.empty() || !ResolveStatisticsEventWindow(statistics, absoluteTimeUs, bounds))
    {
        return;
    }

    const bool firstTransmission = !identity || statistics.seenTaggedMpdus.insert(*identity).second;
    using DirectionViewKey = std::pair<uint32_t, ExperimentDirection>;
    std::map<DirectionViewKey, std::set<uint32_t>> peersByView;
    for (const auto& span : spans)
    {
        if (const auto legacy =
                ResolveLegacyPhyFlow(statistics, span.sourceIpv4, span.destinationIpv4))
        {
            RecordLegacyPhyPayload(statistics, bounds.index, *legacy, span.bytes);
        }

        const auto flow = ResolvePhyFlow(statistics, span.sourceIpv4, span.destinationIpv4);
        if (!flow || flow->sender->nodeId != transmitterNodeId)
        {
            continue;
        }

        auto& senderLocal = statistics.unifiedWindows[bounds.index][flow->sender->nodeId];
        auto& receiverLocal = statistics.unifiedWindows[bounds.index][flow->receiver->nodeId];
        AddPayload(GetPhyDirection(senderLocal, flow->direction),
                   flow->receiver->nodeId,
                   span.bytes,
                   firstTransmission);
        AddPayload(GetPhyDirection(receiverLocal, flow->direction),
                   flow->sender->nodeId,
                   span.bytes,
                   firstTransmission);
        peersByView[{flow->sender->nodeId, flow->direction}].insert(flow->receiver->nodeId);
        peersByView[{flow->receiver->nodeId, flow->direction}].insert(flow->sender->nodeId);

        if (firstTransmission && absoluteTimeUs >= span.applicationTransmitTimeUs)
        {
            senderLocal.applicationToPhyDelayUs.Get(flow->direction)
                .Add(static_cast<double>(absoluteTimeUs - span.applicationTransmitTimeUs));
        }
    }

    for (const auto& [view, peerNodeIds] : peersByView)
    {
        auto& accumulator =
            GetPhyDirection(statistics.unifiedWindows[bounds.index][view.first], view.second);
        ++accumulator.taggedMpduCount;
        accumulator.completeTaggedMpduBytes += completeMpduBytes;
        if (!firstTransmission)
        {
            ++accumulator.retransmissionCount;
            for (uint32_t peerNodeId : peerNodeIds)
            {
                ++accumulator.peersByNodeId[peerNodeId].retransmissionCount;
            }
        }
    }

    // Transitional one-second fields remain the source for the current serializer through Task 7.
    if (auto* legacy = GetLegacyNodeSecond(statistics, transmitterNodeId, absoluteTimeUs))
    {
        ++legacy->phyTaggedMpduCount;
        legacy->phyMpduBytes += completeMpduBytes;
        if (!firstTransmission)
        {
            ++legacy->phyRetransmissions;
        }
        for (const auto& span : spans)
        {
            legacy->phyPayloadBytes += span.bytes;
            if (firstTransmission)
            {
                legacy->phyUniquePayloadBytes += span.bytes;
                if (absoluteTimeUs >= span.applicationTransmitTimeUs)
                {
                    legacy->appToPhy.Add(
                        static_cast<double>(absoluteTimeUs - span.applicationTransmitTimeUs));
                }
            }
        }
    }
}

void
RecordPhyRateAttempt(WifiStatisticsState& statistics,
                     uint32_t transmitterNodeId,
                     int64_t absoluteTimeUs,
                     const std::string& sourceIpv4,
                     const std::string& destinationIpv4,
                     double dataRateBps,
                     long double allocatedAirtimeUs)
{
    ExperimentWindowBounds bounds;
    if (dataRateBps <= 0.0 || allocatedAirtimeUs <= 0.0L ||
        !ResolveStatisticsEventWindow(statistics, absoluteTimeUs, bounds))
    {
        return;
    }

    // Transitional writes intentionally retain the legacy IP maps and do not depend on exact
    // entity identity or callback-transmitter validation through Task 7.
    if (const auto legacy = ResolveLegacyPhyFlow(statistics, sourceIpv4, destinationIpv4))
    {
        RecordLegacyPhyRate(statistics, bounds.index, *legacy, dataRateBps, allocatedAirtimeUs);
    }

    const auto flow = ResolvePhyFlow(statistics, sourceIpv4, destinationIpv4);
    if (!flow || flow->sender->nodeId != transmitterNodeId)
    {
        return;
    }

    auto& sender = statistics.unifiedWindows[bounds.index][flow->sender->nodeId];
    auto& receiver = statistics.unifiedWindows[bounds.index][flow->receiver->nodeId];
    AddRate(GetPhyDirection(sender, flow->direction),
            flow->receiver->nodeId,
            dataRateBps,
            allocatedAirtimeUs);
    AddRate(GetPhyDirection(receiver, flow->direction),
            flow->sender->nodeId,
            dataRateBps,
            allocatedAirtimeUs);
}

void
RecordPhyTxPsduBegin(WifiStatisticsState& statistics,
                     uint32_t transmitterNodeId,
                     int64_t absoluteTimeUs,
                     WifiPhyBand band,
                     const WifiConstPsduMap& psduMap,
                     const WifiTxVector& txVector)
{
    using PpduFlowKey = std::pair<std::string, std::string>;
    std::map<PpduFlowKey, PpduFlowContribution> contributions;
    uint64_t totalTaggedBytes = 0;
    for (const auto& [staId, psdu] : psduMap)
    {
        if (!psdu)
        {
            continue;
        }
        const double rateBps =
            static_cast<double>(txVector.GetMode(staId).GetDataRate(txVector, staId));
        for (const auto& mpdu : *PeekPointer(psdu))
        {
            if (!mpdu || !mpdu->GetHeader().IsData())
            {
                continue;
            }
            auto iterator = mpdu->GetPacket()->GetByteTagIterator();
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
                const std::string sourceIpv4 = Ipv4ToString(tag.GetSource());
                const std::string destinationIpv4 = Ipv4ToString(tag.GetDestination());
                const auto exact = ResolvePhyFlow(statistics, sourceIpv4, destinationIpv4);
                const auto legacy = ResolveLegacyPhyFlow(statistics, sourceIpv4, destinationIpv4);
                if (taggedBytes == 0 || (!exact && !legacy))
                {
                    continue;
                }
                auto& contribution = contributions[{sourceIpv4, destinationIpv4}];
                contribution.taggedBytes += taggedBytes;
                contribution.rateBpsTimesBytes += static_cast<long double>(rateBps) * taggedBytes;
                totalTaggedBytes += taggedBytes;
            }
        }
    }

    if (totalTaggedBytes == 0 || contributions.empty())
    {
        return;
    }
    const long double ppduAirtimeUs =
        static_cast<long double>(
            WifiPhy::CalculateTxDuration(psduMap, txVector, band).GetNanoSeconds()) /
        1000.0L;
    if (ppduAirtimeUs <= 0.0L)
    {
        return;
    }

    for (const auto& [flow, contribution] : contributions)
    {
        const long double allocatedAirtimeUs =
            ppduAirtimeUs * contribution.taggedBytes / totalTaggedBytes;
        const double averageRateBps =
            static_cast<double>(contribution.rateBpsTimesBytes / contribution.taggedBytes);
        RecordPhyRateAttempt(statistics,
                             transmitterNodeId,
                             absoluteTimeUs,
                             flow.first,
                             flow.second,
                             averageRateBps,
                             allocatedAirtimeUs);
    }
}

std::optional<double>
CalculateAveragePhyDataRateMbps(const PhyPeerAccumulator& accumulator)
{
    if (accumulator.transmissionAirtimeUs <= 0.0L)
    {
        return std::nullopt;
    }
    return static_cast<double>(accumulator.dataRateBpsUs / accumulator.transmissionAirtimeUs /
                               1e6L);
}

void
RecordPhyBusyInterval(WifiStatisticsState& statistics,
                      uint32_t nodeId,
                      int64_t absoluteStartUs,
                      int64_t durationUs)
{
    const int64_t experimentStartUs = statistics.coordinator.GetExperimentStartUs();
    if (experimentStartUs < 0 || durationUs <= 0)
    {
        return;
    }

    const int64_t relativeStartUs = absoluteStartUs - experimentStartUs;
    const int64_t relativeEndUs = relativeStartUs + durationUs;
    const int64_t experimentDurationUs = GetExperimentDurationUs(statistics);
    if (statistics.entityRegistry.FindByNodeId(nodeId))
    {
        for (const auto& [windowIndex, overlapUs] : SplitExperimentInterval(relativeStartUs,
                                                                            relativeEndUs,
                                                                            experimentDurationUs,
                                                                            statistics.windowUs))
        {
            statistics.unifiedWindows[windowIndex][nodeId].phy.busyTimeUs += overlapUs;
        }
    }

    // Transitional fixed-second fields remain the source for the current serializer through Task 7.
    for (const auto& [secondIndex, overlapUs] :
         SplitExperimentInterval(relativeStartUs, relativeEndUs, experimentDurationUs, 1000000))
    {
        statistics.nodeSeconds[nodeId][secondIndex].phyBusyUs += overlapUs;
    }
}

void
ConnectPhyTraces(WifiStatisticsState& statistics, uint32_t nodeId, Ptr<WifiNetDevice> device)
{
    device->GetPhy()->TraceConnectWithoutContext(
        "PhyTxBegin",
        MakeBoundCallback(&PhyTxBeginTrace, &statistics, nodeId));
    device->GetPhy()->TraceConnectWithoutContext("PhyTxPsduBegin",
                                                 MakeBoundCallback(&PhyTxPsduBeginTrace,
                                                                   &statistics,
                                                                   nodeId,
                                                                   device->GetPhy()->GetPhyBand()));
    device->GetPhy()->GetState()->TraceConnectWithoutContext(
        "State",
        MakeBoundCallback(&PhyStateTrace, &statistics, nodeId));
}

} // namespace ns3
