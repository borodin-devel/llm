#include "wifi-statistics-internal.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

namespace ns3
{

namespace
{

/**
 * Compare nonnegative raw floating-point accumulators with a fixed roundoff bound.
 *
 * The 64-epsilon relative bound covers only low-bit changes caused by reassociating a modest
 * number of additions. Integer raw totals remain exact, and non-finite unequal values never match.
 */
bool
RawLongDoubleEqual(long double left, long double right)
{
    if (left == right)
    {
        return true;
    }
    if (!std::isfinite(left) || !std::isfinite(right))
    {
        return false;
    }
    const long double scale = std::max(std::fabs(left), std::fabs(right));
    const long double bound = 64.0L * std::numeric_limits<long double>::epsilon() * scale;
    return std::fabs(left - right) <= bound;
}

template <typename Map, typename Equal>
bool
RawMapEqual(const Map& left, const Map& right, Equal equal)
{
    if (left.size() != right.size())
    {
        return false;
    }
    auto leftIterator = left.begin();
    auto rightIterator = right.begin();
    while (leftIterator != left.end())
    {
        if (leftIterator->first != rightIterator->first ||
            !equal(leftIterator->second, rightIterator->second))
        {
            return false;
        }
        ++leftIterator;
        ++rightIterator;
    }
    return true;
}

bool
RawSampleEqual(const SampleAccumulator& left, const SampleAccumulator& right)
{
    return left.count == right.count && RawLongDoubleEqual(left.sum, right.sum) &&
           RawLongDoubleEqual(left.sumSquares, right.sumSquares) && left.minimum == right.minimum &&
           left.maximum == right.maximum;
}

bool
RawAppEqual(const AppDirectionAccumulator& left, const AppDirectionAccumulator& right)
{
    return left.acceptedSendCount == right.acceptedSendCount &&
           left.acceptedPayloadBytes == right.acceptedPayloadBytes &&
           left.receiveEventCount == right.receiveEventCount &&
           left.receivedPayloadBytes == right.receivedPayloadBytes &&
           left.dropEventCount == right.dropEventCount &&
           left.droppedPayloadBytes == right.droppedPayloadBytes &&
           RawSampleEqual(left.receiveInterArrivalUs, right.receiveInterArrivalUs) &&
           left.agents == right.agents && left.peersByNodeId == right.peersByNodeId;
}

bool
RawDeviceEqual(const DeviceTransmissionAccumulator& left,
               const DeviceTransmissionAccumulator& right)
{
    return left.estimatedTransmittedTcpPayloadBytes == right.estimatedTransmittedTcpPayloadBytes &&
           left.estimatedMatchedTcpPayloadBytes == right.estimatedMatchedTcpPayloadBytes &&
           left.matchedPacketCount == right.matchedPacketCount &&
           RawSampleEqual(left.transmissionDurationUs, right.transmissionDurationUs);
}

bool
RawPhyPeerEqual(const PhyPeerAccumulator& left, const PhyPeerAccumulator& right)
{
    return left.taggedPayloadBytes == right.taggedPayloadBytes &&
           left.uniqueTaggedPayloadBytes == right.uniqueTaggedPayloadBytes &&
           left.transmissionAttemptCount == right.transmissionAttemptCount &&
           left.retransmissionCount == right.retransmissionCount &&
           RawLongDoubleEqual(left.dataRateBpsUs, right.dataRateBpsUs) &&
           RawLongDoubleEqual(left.transmissionAirtimeUs, right.transmissionAirtimeUs);
}

bool
RawPhyEqual(const PhyDirectionAccumulator& left, const PhyDirectionAccumulator& right)
{
    return RawPhyPeerEqual(left, right) && left.taggedMpduCount == right.taggedMpduCount &&
           left.completeTaggedMpduBytes == right.completeTaggedMpduBytes &&
           RawMapEqual(left.peersByNodeId, right.peersByNodeId, RawPhyPeerEqual);
}

bool
RawTcpEqual(const TcpWindowAccumulator& left, const TcpWindowAccumulator& right)
{
    return RawLongDoubleEqual(left.congestionWindowBytesUs, right.congestionWindowBytesUs) &&
           left.congestionWindowObservationDurationUs ==
               right.congestionWindowObservationDurationUs &&
           left.lastCongestionWindowBytes == right.lastCongestionWindowBytes &&
           RawSampleEqual(left.roundTripTimeUs, right.roundTripTimeUs);
}

bool
RawEntityEqual(const LocalEntityWindowAccumulator& left, const LocalEntityWindowAccumulator& right)
{
    return RawAppEqual(left.app.uplink, right.app.uplink) &&
           RawAppEqual(left.app.downlink, right.app.downlink) &&
           RawDeviceEqual(left.deviceTransmission.uplink, right.deviceTransmission.uplink) &&
           RawDeviceEqual(left.deviceTransmission.downlink, right.deviceTransmission.downlink) &&
           RawSampleEqual(left.applicationToPhyDelayUs.uplink,
                          right.applicationToPhyDelayUs.uplink) &&
           RawSampleEqual(left.applicationToPhyDelayUs.downlink,
                          right.applicationToPhyDelayUs.downlink) &&
           left.mac == right.mac && left.phy.busyTimeUs == right.phy.busyTimeUs &&
           RawPhyEqual(left.phy.uplink, right.phy.uplink) &&
           RawPhyEqual(left.phy.downlink, right.phy.downlink) &&
           RawMapEqual(left.tcpConnections, right.tcpConnections, RawTcpEqual);
}

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
    return RawPhyPeerEqual(total, direction);
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
PhyDirectionTotalsEqual(const PhyDirectionAccumulator& parent,
                        const PhyDirectionAccumulator& children)
{
    return RawPhyPeerEqual(parent, children) &&
           parent.taggedMpduCount == children.taggedMpduCount &&
           parent.completeTaggedMpduBytes == children.completeTaggedMpduBytes;
}

LocalEntityWindowAccumulator
ReconstructAccessPoint(const UnifiedEntityAccumulatorMap& localEntities,
                       const ExperimentEntityIdentity& accessPoint,
                       const ExperimentEntityRegistry& registry)
{
    LocalEntityWindowAccumulator expected;
    if (const auto local = localEntities.find(accessPoint.nodeId); local != localEntities.end())
    {
        expected = local->second;
    }
    for (const auto& stationIdentity : registry.GetStations())
    {
        if (stationIdentity.accessPointId != accessPoint.accessPointId)
        {
            continue;
        }
        const auto station = localEntities.find(stationIdentity.nodeId);
        if (station == localEntities.end())
        {
            continue;
        }
        MergeStationIntoAccessPoint(expected,
                                    station->second,
                                    accessPoint.nodeId,
                                    stationIdentity.nodeId);
    }
    return expected;
}

bool
AttributedPhyMatchesChildren(const LocalEntityWindowAccumulator& parent,
                             const UnifiedEntityAccumulatorMap& localEntities,
                             uint32_t accessPointId,
                             const ExperimentEntityRegistry& registry)
{
    LocalEntityWindowAccumulator children;
    for (const auto& stationIdentity : registry.GetStations())
    {
        if (stationIdentity.accessPointId != accessPointId)
        {
            continue;
        }
        if (const auto station = localEntities.find(stationIdentity.nodeId);
            station != localEntities.end())
        {
            MergeLocalEntityWindowAccumulator(children, station->second);
        }
    }
    return PhyDirectionTotalsEqual(parent.phy.uplink, children.phy.uplink) &&
           PhyDirectionTotalsEqual(parent.phy.downlink, children.phy.downlink);
}

bool
ParentTotalsConsistent(const UnifiedSummaryRawState& raw, const ExperimentEntityRegistry& registry)
{
    if (raw.accessPointWindows.size() != raw.localWindows.size())
    {
        return false;
    }

    UnifiedEntityAccumulatorMap expectedOverall;
    for (const auto& accessPoint : registry.GetAccessPoints())
    {
        expectedOverall[accessPoint.nodeId];
    }
    for (const auto& [windowIndex, localEntities] : raw.localWindows)
    {
        const auto materializedWindow = raw.accessPointWindows.find(windowIndex);
        if (materializedWindow == raw.accessPointWindows.end() ||
            materializedWindow->second.size() != registry.GetAccessPoints().size())
        {
            return false;
        }
        for (const auto& accessPoint : registry.GetAccessPoints())
        {
            const auto actual = materializedWindow->second.find(accessPoint.nodeId);
            if (actual == materializedWindow->second.end())
            {
                return false;
            }
            const auto expected = ReconstructAccessPoint(localEntities, accessPoint, registry);
            if (!RawEntityEqual(actual->second, expected) ||
                !AttributedPhyMatchesChildren(expected,
                                              localEntities,
                                              accessPoint.accessPointId,
                                              registry))
            {
                return false;
            }
            MergeLocalEntityWindowAccumulator(expectedOverall[accessPoint.nodeId], expected);
        }
    }
    return RawMapEqual(expectedOverall, raw.accessPointOverall, RawEntityEqual);
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
    return RawMapEqual(expected, overall, RawEntityEqual);
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
