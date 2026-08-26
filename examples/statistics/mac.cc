#include "../runtime/traffic-coordinator.h"
#include "internal.h"

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
GetMacAccumulator(ExperimentStatisticsState& statistics, uint32_t nodeId, int64_t absoluteTimeUs)
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

std::optional<uint32_t>
ResolvePeerNodeId(const ExperimentStatisticsState& statistics, Mac48Address peer)
{
    const auto iterator = statistics.nodeIdsByMacAddress.find(peer);
    return iterator == statistics.nodeIdsByMacAddress.end()
               ? std::nullopt
               : std::optional<uint32_t>(iterator->second);
}

void
MacTxDropTrace(ExperimentStatisticsState* statistics, uint32_t nodeId, Ptr<const Packet> packet)
{
    RecordMacTransmitDrop(*statistics,
                          nodeId,
                          Simulator::Now().GetMicroSeconds(),
                          packet->GetSize());
}

void
MacDroppedMpduTrace(ExperimentStatisticsState* statistics,
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
MacTxDataFailedTrace(ExperimentStatisticsState* statistics, uint32_t nodeId, Mac48Address remote)
{
    RecordMacDataFailure(*statistics,
                         nodeId,
                         Simulator::Now().GetMicroSeconds(),
                         false,
                         ResolvePeerNodeId(*statistics, remote));
}

void
MacTxFinalDataFailedTrace(ExperimentStatisticsState* statistics,
                          uint32_t nodeId,
                          Mac48Address remote)
{
    RecordMacDataFailure(*statistics,
                         nodeId,
                         Simulator::Now().GetMicroSeconds(),
                         true,
                         ResolvePeerNodeId(*statistics, remote));
}

} // namespace

void
RecordMacTransmitDrop(ExperimentStatisticsState& statistics,
                      uint32_t nodeId,
                      int64_t absoluteTimeUs,
                      uint32_t packetBytes)
{
    if (auto* accumulator = GetMacAccumulator(statistics, nodeId, absoluteTimeUs))
    {
        ++accumulator->transmitDropCount;
        accumulator->transmitDropPacketBytes += packetBytes;
    }
}

void
RecordMacMpduDrop(ExperimentStatisticsState& statistics,
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
}

void
RecordMacDataFailure(ExperimentStatisticsState& statistics,
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
}

void
ConnectMacTraces(ExperimentStatisticsState& statistics, uint32_t nodeId, Ptr<WifiNetDevice> device)
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
