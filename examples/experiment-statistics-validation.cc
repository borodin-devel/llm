#include "wifi-statistics-internal.h"

#include <map>

namespace ns3
{

namespace
{

template <typename Callback>
void
ForEachMaterializedEntity(const UnifiedSummaryRawState& raw, Callback callback)
{
    for (const auto& [windowIndex, entities] : raw.accessPointWindows)
    {
        (void)windowIndex;
        for (const auto& [nodeId, entity] : entities)
        {
            callback(nodeId, entity);
        }
    }
    for (const auto& [windowIndex, entities] : raw.stationWindows)
    {
        (void)windowIndex;
        for (const auto& [nodeId, entity] : entities)
        {
            callback(nodeId, entity);
        }
    }
    for (const auto& [nodeId, entity] : raw.accessPointOverall)
    {
        callback(nodeId, entity);
    }
    for (const auto& [nodeId, entity] : raw.stationOverall)
    {
        callback(nodeId, entity);
    }
}

bool
IsValidPeer(const ExperimentEntityRegistry& registry, uint32_t ownerNodeId, uint32_t peerNodeId)
{
    const auto* owner = registry.FindByNodeId(ownerNodeId);
    const auto* peer = registry.FindByNodeId(peerNodeId);
    return owner && peer && owner->kind != peer->kind &&
           owner->accessPointId == peer->accessPointId;
}

bool
EntityReferencesValid(const ExperimentEntityRegistry& registry,
                      uint32_t ownerNodeId,
                      const LocalEntityWindowAccumulator& entity)
{
    if (!registry.FindByNodeId(ownerNodeId))
    {
        return false;
    }
    const auto CheckPeers = [&](const auto& peers) {
        for (const auto& [peerNodeId, value] : peers)
        {
            (void)value;
            if (!IsValidPeer(registry, ownerNodeId, peerNodeId))
            {
                return false;
            }
        }
        return true;
    };
    if (!CheckPeers(entity.app.uplink.peersByNodeId) ||
        !CheckPeers(entity.app.downlink.peersByNodeId) ||
        !CheckPeers(entity.mac.uplink.peersByNodeId) ||
        !CheckPeers(entity.mac.downlink.peersByNodeId) ||
        !CheckPeers(entity.phy.uplink.peersByNodeId) ||
        !CheckPeers(entity.phy.downlink.peersByNodeId))
    {
        return false;
    }
    for (const auto& [key, value] : entity.tcpConnections)
    {
        (void)value;
        if (!IsValidPeer(registry, ownerNodeId, key.second))
        {
            return false;
        }
    }
    return true;
}

bool
AppAgentsConsistent(const AppDirectionAccumulator& direction)
{
    uint64_t acceptedCount = 0;
    uint64_t acceptedBytes = 0;
    uint64_t dropCount = 0;
    uint64_t dropBytes = 0;
    for (const auto& [key, agent] : direction.agents)
    {
        (void)key;
        acceptedCount += agent.acceptedSendCount;
        acceptedBytes += agent.acceptedPayloadBytes;
        dropCount += agent.dropEventCount;
        dropBytes += agent.droppedPayloadBytes;
    }
    return acceptedCount == direction.acceptedSendCount &&
           acceptedBytes == direction.acceptedPayloadBytes &&
           dropCount == direction.dropEventCount && dropBytes == direction.droppedPayloadBytes;
}

bool
Consume(uint64_t& remaining, uint64_t value)
{
    if (value > remaining)
    {
        return false;
    }
    remaining -= value;
    return true;
}

bool
AppPeersConsistent(const AppDirectionAccumulator& direction)
{
    uint64_t acceptedCount = direction.acceptedSendCount;
    uint64_t acceptedBytes = direction.acceptedPayloadBytes;
    uint64_t receiveCount = direction.receiveEventCount;
    uint64_t receiveBytes = direction.receivedPayloadBytes;
    uint64_t dropCount = direction.dropEventCount;
    uint64_t dropBytes = direction.droppedPayloadBytes;
    for (const auto& [nodeId, peer] : direction.peersByNodeId)
    {
        (void)nodeId;
        if (!Consume(acceptedCount, peer.acceptedSendCount) ||
            !Consume(acceptedBytes, peer.acceptedPayloadBytes) ||
            !Consume(receiveCount, peer.receiveEventCount) ||
            !Consume(receiveBytes, peer.receivedPayloadBytes) ||
            !Consume(dropCount, peer.dropEventCount) ||
            !Consume(dropBytes, peer.droppedPayloadBytes))
        {
            return false;
        }
    }
    return true;
}

uint64_t
SumReasons(const std::map<int, uint64_t>& reasons)
{
    uint64_t total = 0;
    for (const auto& [reason, count] : reasons)
    {
        (void)reason;
        total += count;
    }
    return total;
}

bool
MacPeersConsistent(const MacDirectionAccumulator& direction)
{
    if (SumReasons(direction.mpduDropsByReason) != direction.mpduDropCount)
    {
        return false;
    }
    uint64_t transmitCount = direction.estimatedTransmitEventCount;
    uint64_t transmitBytes = direction.estimatedTransmittedTcpPayloadBytes;
    uint64_t receiveCount = direction.estimatedReceiveEventCount;
    uint64_t receiveBytes = direction.estimatedReceivedTcpPayloadBytes;
    uint64_t mpduCount = direction.mpduDropCount;
    uint64_t mpduBytes = direction.mpduDropBytes;
    uint64_t failureCount = direction.dataFailureCount;
    uint64_t finalFailureCount = direction.finalDataFailureCount;
    for (const auto& [nodeId, peer] : direction.peersByNodeId)
    {
        (void)nodeId;
        if (SumReasons(peer.mpduDropsByReason) != peer.mpduDropCount ||
            !Consume(transmitCount, peer.estimatedTransmitEventCount) ||
            !Consume(transmitBytes, peer.estimatedTransmittedTcpPayloadBytes) ||
            !Consume(receiveCount, peer.estimatedReceiveEventCount) ||
            !Consume(receiveBytes, peer.estimatedReceivedTcpPayloadBytes) ||
            !Consume(mpduCount, peer.mpduDropCount) || !Consume(mpduBytes, peer.mpduDropBytes) ||
            !Consume(failureCount, peer.dataFailureCount) ||
            !Consume(finalFailureCount, peer.finalDataFailureCount))
        {
            return false;
        }
    }
    return true;
}

bool
PhyPeersConsistent(const PhyDirectionAccumulator& direction)
{
    PhyPeerAccumulator total;
    for (const auto& [nodeId, peer] : direction.peersByNodeId)
    {
        (void)nodeId;
        total.taggedPayloadBytes += peer.taggedPayloadBytes;
        total.uniqueTaggedPayloadBytes += peer.uniqueTaggedPayloadBytes;
        total.transmissionAttemptCount += peer.transmissionAttemptCount;
        total.retransmissionCount += peer.retransmissionCount;
        total.dataRateBpsUs += peer.dataRateBpsUs;
        total.transmissionAirtimeUs += peer.transmissionAirtimeUs;
    }
    return total == static_cast<const PhyPeerAccumulator&>(direction);
}

bool
PhyUniqueValid(const PhyDirectionAccumulator& direction)
{
    if (direction.uniqueTaggedPayloadBytes > direction.taggedPayloadBytes)
    {
        return false;
    }
    for (const auto& [nodeId, peer] : direction.peersByNodeId)
    {
        (void)nodeId;
        if (peer.uniqueTaggedPayloadBytes > peer.taggedPayloadBytes)
        {
            return false;
        }
    }
    return true;
}

bool
AppSenderEqual(const AppDirectionAccumulator& parent, const AppDirectionAccumulator& children)
{
    return parent.acceptedSendCount == children.acceptedSendCount &&
           parent.acceptedPayloadBytes == children.acceptedPayloadBytes &&
           parent.dropEventCount == children.dropEventCount &&
           parent.droppedPayloadBytes == children.droppedPayloadBytes;
}

bool
MacSenderEqual(const MacDirectionAccumulator& parent, const MacDirectionAccumulator& children)
{
    return parent.estimatedTransmitEventCount == children.estimatedTransmitEventCount &&
           parent.estimatedTransmittedTcpPayloadBytes ==
               children.estimatedTransmittedTcpPayloadBytes &&
           parent.transmitDropCount == children.transmitDropCount &&
           parent.transmitDropPacketBytes == children.transmitDropPacketBytes &&
           parent.mpduDropCount == children.mpduDropCount &&
           parent.mpduDropBytes == children.mpduDropBytes &&
           parent.dataFailureCount == children.dataFailureCount &&
           parent.finalDataFailureCount == children.finalDataFailureCount &&
           parent.mpduDropsByReason == children.mpduDropsByReason;
}

bool
PhyDirectionTotalsEqual(const PhyDirectionAccumulator& parent,
                        const PhyDirectionAccumulator& children)
{
    return static_cast<const PhyPeerAccumulator&>(parent) ==
               static_cast<const PhyPeerAccumulator&>(children) &&
           parent.taggedMpduCount == children.taggedMpduCount &&
           parent.completeTaggedMpduBytes == children.completeTaggedMpduBytes;
}

bool
TcpUplinkEqual(const LocalEntityWindowAccumulator& parent,
               const UnifiedEntityAccumulatorMap& stations,
               uint32_t accessPointId,
               const ExperimentEntityRegistry& registry)
{
    std::map<uint32_t, TcpWindowAccumulator> expected;
    for (const auto& stationIdentity : registry.GetStations())
    {
        if (stationIdentity.accessPointId != accessPointId)
        {
            continue;
        }
        const auto station = stations.find(stationIdentity.nodeId);
        if (station == stations.end())
        {
            continue;
        }
        for (const auto& [key, connection] : station->second.tcpConnections)
        {
            if (key.first != ExperimentDirection::UPLINK)
            {
                continue;
            }
            auto& target = expected[stationIdentity.nodeId];
            target.congestionWindowBytesUs += connection.congestionWindowBytesUs;
            target.congestionWindowObservationDurationUs +=
                connection.congestionWindowObservationDurationUs;
            if (connection.lastCongestionWindowBytes)
            {
                target.lastCongestionWindowBytes = connection.lastCongestionWindowBytes;
            }
            target.roundTripTimeUs.Merge(connection.roundTripTimeUs);
        }
    }
    std::map<uint32_t, TcpWindowAccumulator> actual;
    for (const auto& [key, connection] : parent.tcpConnections)
    {
        if (key.first == ExperimentDirection::UPLINK)
        {
            actual[key.second] = connection;
        }
    }
    return actual == expected;
}

bool
ParentTotalsForMaps(const UnifiedEntityAccumulatorMap& accessPoints,
                    const UnifiedEntityAccumulatorMap& stations,
                    const ExperimentEntityRegistry& registry)
{
    for (const auto& accessPointIdentity : registry.GetAccessPoints())
    {
        const auto parentIterator = accessPoints.find(accessPointIdentity.nodeId);
        const LocalEntityWindowAccumulator empty;
        const auto& parent = parentIterator == accessPoints.end() ? empty : parentIterator->second;
        LocalEntityWindowAccumulator children;
        for (const auto& stationIdentity : registry.GetStations())
        {
            if (stationIdentity.accessPointId != accessPointIdentity.accessPointId)
            {
                continue;
            }
            if (const auto station = stations.find(stationIdentity.nodeId);
                station != stations.end())
            {
                MergeLocalEntityWindowAccumulator(children, station->second);
            }
        }
        if (!(parent.deviceTransmission.uplink == children.deviceTransmission.uplink) ||
            !(parent.applicationToPhyDelayUs.uplink == children.applicationToPhyDelayUs.uplink) ||
            !AppSenderEqual(parent.app.uplink, children.app.uplink) ||
            !MacSenderEqual(parent.mac.uplink, children.mac.uplink) ||
            !PhyDirectionTotalsEqual(parent.phy.uplink, children.phy.uplink) ||
            !PhyDirectionTotalsEqual(parent.phy.downlink, children.phy.downlink) ||
            !TcpUplinkEqual(parent, stations, accessPointIdentity.accessPointId, registry))
        {
            return false;
        }
    }
    return true;
}

bool
ParentTotalsConsistent(const UnifiedSummaryRawState& raw, const ExperimentEntityRegistry& registry)
{
    for (const auto& [windowIndex, accessPoints] : raw.accessPointWindows)
    {
        const auto stations = raw.stationWindows.find(windowIndex);
        const UnifiedEntityAccumulatorMap empty;
        if (!ParentTotalsForMaps(accessPoints,
                                 stations == raw.stationWindows.end() ? empty : stations->second,
                                 registry))
        {
            return false;
        }
    }
    return ParentTotalsForMaps(raw.accessPointOverall, raw.stationOverall, registry);
}

bool
OverallForMapsMatches(const UnifiedExperimentWindowStore& windows,
                      const UnifiedEntityAccumulatorMap& overall,
                      const std::vector<ExperimentEntityIdentity>& inventory)
{
    UnifiedEntityAccumulatorMap expected;
    for (const auto& identity : inventory)
    {
        expected[identity.nodeId];
    }
    for (const auto& [windowIndex, entities] : windows)
    {
        (void)windowIndex;
        for (const auto& [nodeId, entity] : entities)
        {
            MergeLocalEntityWindowAccumulator(expected[nodeId], entity);
        }
    }
    return expected == overall;
}

} // namespace

ExperimentValidationOutput
ValidateUnifiedSummaryRawState(const ExperimentEntityRegistry& registry,
                               const UnifiedSummaryRawState& raw)
{
    ExperimentValidationOutput output;
    for (const auto& [windowIndex, entities] : raw.localWindows)
    {
        (void)windowIndex;
        for (const auto& [nodeId, entity] : entities)
        {
            output.entityInventoryReferencesValid &=
                EntityReferencesValid(registry, nodeId, entity);
        }
    }
    ForEachMaterializedEntity(raw, [&](uint32_t nodeId, const auto& entity) {
        output.entityInventoryReferencesValid &= EntityReferencesValid(registry, nodeId, entity);
        output.appAgentTotalsConsistent &=
            AppAgentsConsistent(entity.app.uplink) && AppAgentsConsistent(entity.app.downlink);
        output.appPeerTotalsConsistent &=
            AppPeersConsistent(entity.app.uplink) && AppPeersConsistent(entity.app.downlink);
        output.macPeerTotalsConsistent &=
            MacPeersConsistent(entity.mac.uplink) && MacPeersConsistent(entity.mac.downlink);
        output.phyPeerTotalsConsistent &=
            PhyPeersConsistent(entity.phy.uplink) && PhyPeersConsistent(entity.phy.downlink);
        output.uniquePhyPayloadWithinTaggedPayload &=
            PhyUniqueValid(entity.phy.uplink) && PhyUniqueValid(entity.phy.downlink);
    });
    output.apStationSenderTotalsConsistent = ParentTotalsConsistent(raw, registry);
    output.overallMatchesWindows =
        OverallForMapsMatches(raw.accessPointWindows,
                              raw.accessPointOverall,
                              registry.GetAccessPoints()) &&
        OverallForMapsMatches(raw.stationWindows, raw.stationOverall, registry.GetStations());
    return output;
}

} // namespace ns3
