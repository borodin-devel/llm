#include "traffic-coordinator.h"
#include "wifi-statistics-internal.h"

#include "ns3/packet.h"
#include "ns3/simulator.h"
#include "ns3/wifi-module.h"

#include <cmath>
#include <optional>

namespace ns3
{

namespace
{

ExperimentDirection
GetTransmitterDirection(const ExperimentEntityIdentity& entity)
{
    return entity.kind == ExperimentEntityKind::ACCESS_POINT ? ExperimentDirection::DOWNLINK
                                                             : ExperimentDirection::UPLINK;
}

MacDirectionAccumulator*
GetMacAccumulator(WifiStatisticsState& statistics, uint32_t nodeId, int64_t absoluteTimeUs)
{
    ExperimentWindowBounds bounds;
    const auto* entity = statistics.entityRegistry.FindByNodeId(nodeId);
    if (!entity || !ResolveStatisticsEventWindow(statistics, absoluteTimeUs, bounds))
    {
        return nullptr;
    }
    return &statistics.unifiedWindows[bounds.index][nodeId].mac.Get(
        GetTransmitterDirection(*entity));
}

NodeSecondStats*
GetLegacyNodeSecond(WifiStatisticsState& statistics, uint32_t nodeId, int64_t absoluteTimeUs)
{
    const int64_t experimentStartUs = statistics.coordinator.GetExperimentStartUs();
    if (experimentStartUs < 0)
    {
        return nullptr;
    }
    const int64_t durationUs = static_cast<int64_t>(
        std::ceil(statistics.coordinator.GetMaxExperimentDurationMs() * 1000.0));
    uint64_t secondIndex = 0;
    if (!GetNodeSecondIndex(absoluteTimeUs - experimentStartUs, durationUs, secondIndex))
    {
        return nullptr;
    }
    return &statistics.nodeSeconds[nodeId][secondIndex];
}

std::optional<uint32_t>
ResolvePeerNodeId(const WifiStatisticsState& statistics, Mac48Address peer)
{
    const auto iterator = statistics.nodeIdsByMacAddress.find(peer);
    return iterator == statistics.nodeIdsByMacAddress.end()
               ? std::nullopt
               : std::optional<uint32_t>(iterator->second);
}

void
MacTxDropTrace(WifiStatisticsState* statistics, uint32_t nodeId, Ptr<const Packet> packet)
{
    RecordMacTransmitDrop(*statistics,
                          nodeId,
                          Simulator::Now().GetMicroSeconds(),
                          packet->GetSize());
}

void
MacDroppedMpduTrace(WifiStatisticsState* statistics,
                    uint32_t nodeId,
                    WifiMacDropReason reason,
                    Ptr<const WifiMpdu> mpdu)
{
    const std::optional<uint32_t> peerNodeId =
        mpdu ? ResolvePeerNodeId(*statistics, mpdu->GetHeader().GetAddr1()) : std::nullopt;
    RecordMacMpduDrop(*statistics,
                      nodeId,
                      Simulator::Now().GetMicroSeconds(),
                      static_cast<int>(reason),
                      mpdu ? mpdu->GetSize() : 0,
                      peerNodeId);
}

void
MacTxDataFailedTrace(WifiStatisticsState* statistics, uint32_t nodeId, Mac48Address remote)
{
    RecordMacDataFailure(*statistics,
                         nodeId,
                         Simulator::Now().GetMicroSeconds(),
                         false,
                         ResolvePeerNodeId(*statistics, remote));
}

void
MacTxFinalDataFailedTrace(WifiStatisticsState* statistics, uint32_t nodeId, Mac48Address remote)
{
    RecordMacDataFailure(*statistics,
                         nodeId,
                         Simulator::Now().GetMicroSeconds(),
                         true,
                         ResolvePeerNodeId(*statistics, remote));
}

} // namespace

void
RecordMacTransmitDrop(WifiStatisticsState& statistics,
                      uint32_t nodeId,
                      int64_t absoluteTimeUs,
                      uint32_t packetBytes)
{
    if (auto* accumulator = GetMacAccumulator(statistics, nodeId, absoluteTimeUs))
    {
        ++accumulator->transmitDropCount;
        accumulator->transmitDropPacketBytes += packetBytes;
    }
    if (auto* legacy = GetLegacyNodeSecond(statistics, nodeId, absoluteTimeUs))
    {
        ++legacy->macTxDrops;
        legacy->macTxDropBytes += packetBytes;
    }
}

void
RecordMacMpduDrop(WifiStatisticsState& statistics,
                  uint32_t nodeId,
                  int64_t absoluteTimeUs,
                  int reasonCode,
                  uint32_t mpduBytes,
                  std::optional<uint32_t> peerNodeId)
{
    if (auto* accumulator = GetMacAccumulator(statistics, nodeId, absoluteTimeUs))
    {
        ++accumulator->mpduDropCount;
        accumulator->mpduDropBytes += mpduBytes;
        ++accumulator->mpduDropsByReason[reasonCode];
        if (peerNodeId)
        {
            auto& peer = accumulator->peersByNodeId[*peerNodeId];
            ++peer.mpduDropCount;
            peer.mpduDropBytes += mpduBytes;
            ++peer.mpduDropsByReason[reasonCode];
        }
    }
    if (auto* legacy = GetLegacyNodeSecond(statistics, nodeId, absoluteTimeUs))
    {
        ++legacy->macMpduDrops;
        legacy->macMpduDropBytes += mpduBytes;
        ++legacy->macMpduDropsByReason[reasonCode];
    }
}

void
RecordMacDataFailure(WifiStatisticsState& statistics,
                     uint32_t nodeId,
                     int64_t absoluteTimeUs,
                     bool finalFailure,
                     std::optional<uint32_t> peerNodeId)
{
    if (auto* accumulator = GetMacAccumulator(statistics, nodeId, absoluteTimeUs))
    {
        if (finalFailure)
        {
            ++accumulator->finalDataFailureCount;
        }
        else
        {
            ++accumulator->dataFailureCount;
        }
        if (peerNodeId)
        {
            auto& peer = accumulator->peersByNodeId[*peerNodeId];
            if (finalFailure)
            {
                ++peer.finalDataFailureCount;
            }
            else
            {
                ++peer.dataFailureCount;
            }
        }
    }
    if (auto* legacy = GetLegacyNodeSecond(statistics, nodeId, absoluteTimeUs))
    {
        if (finalFailure)
        {
            ++legacy->macFinalDataFailures;
        }
        else
        {
            ++legacy->macDataFailures;
        }
    }
}

void
ConnectMacTraces(WifiStatisticsState& statistics, uint32_t nodeId, Ptr<WifiNetDevice> device)
{
    statistics.nodeIdsByMacAddress[device->GetMac()->GetAddress()] = nodeId;
    device->GetMac()->TraceConnectWithoutContext(
        "MacTxDrop",
        MakeBoundCallback(&MacTxDropTrace, &statistics, nodeId));
    device->GetMac()->TraceConnectWithoutContext(
        "DroppedMpdu",
        MakeBoundCallback(&MacDroppedMpduTrace, &statistics, nodeId));

    Ptr<WifiRemoteStationManager> manager = device->GetRemoteStationManager();
    manager->TraceConnectWithoutContext(
        "MacTxDataFailed",
        MakeBoundCallback(&MacTxDataFailedTrace, &statistics, nodeId));
    manager->TraceConnectWithoutContext(
        "MacTxFinalDataFailed",
        MakeBoundCallback(&MacTxFinalDataFailedTrace, &statistics, nodeId));
}

} // namespace ns3
