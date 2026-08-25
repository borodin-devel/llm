#include "experiment-statistics-types.h"

#include <algorithm>
#include <stdexcept>
#include <tuple>

namespace ns3
{

namespace
{

template <typename T>
void
AddValue(T& target, const T& source)
{
    target += source;
}

void
MergeAppAgent(AppAgentAccumulator& target, const AppAgentAccumulator& source)
{
    target.acceptedSendCount += source.acceptedSendCount;
    target.acceptedPayloadBytes += source.acceptedPayloadBytes;
    target.dropEventCount += source.dropEventCount;
    target.droppedPayloadBytes += source.droppedPayloadBytes;
}

void
MergeAppPeer(AppPeerAccumulator& target, const AppPeerAccumulator& source)
{
    target.acceptedSendCount += source.acceptedSendCount;
    target.acceptedPayloadBytes += source.acceptedPayloadBytes;
    target.receiveEventCount += source.receiveEventCount;
    target.receivedPayloadBytes += source.receivedPayloadBytes;
    target.dropEventCount += source.dropEventCount;
    target.droppedPayloadBytes += source.droppedPayloadBytes;
}

void
MergeApp(AppDirectionAccumulator& target, const AppDirectionAccumulator& source)
{
    target.acceptedSendCount += source.acceptedSendCount;
    target.acceptedPayloadBytes += source.acceptedPayloadBytes;
    target.receiveEventCount += source.receiveEventCount;
    target.receivedPayloadBytes += source.receivedPayloadBytes;
    target.dropEventCount += source.dropEventCount;
    target.droppedPayloadBytes += source.droppedPayloadBytes;
    target.receiveInterArrivalUs.Merge(source.receiveInterArrivalUs);
    for (const auto& [key, agent] : source.agents)
    {
        MergeAppAgent(target.agents[key], agent);
    }
    for (const auto& [nodeId, peer] : source.peersByNodeId)
    {
        MergeAppPeer(target.peersByNodeId[nodeId], peer);
    }
}

void
MergeDevice(DeviceTransmissionAccumulator& target, const DeviceTransmissionAccumulator& source)
{
    target.estimatedTransmittedTcpPayloadBytes += source.estimatedTransmittedTcpPayloadBytes;
    target.estimatedMatchedTcpPayloadBytes += source.estimatedMatchedTcpPayloadBytes;
    target.matchedPacketCount += source.matchedPacketCount;
    target.transmissionDurationUs.Merge(source.transmissionDurationUs);
}

void
MergeMacPeer(MacPeerAccumulator& target, const MacPeerAccumulator& source)
{
    target.estimatedTransmitEventCount += source.estimatedTransmitEventCount;
    target.estimatedTransmittedTcpPayloadBytes += source.estimatedTransmittedTcpPayloadBytes;
    target.estimatedReceiveEventCount += source.estimatedReceiveEventCount;
    target.estimatedReceivedTcpPayloadBytes += source.estimatedReceivedTcpPayloadBytes;
    target.mpduDropCount += source.mpduDropCount;
    target.mpduDropBytes += source.mpduDropBytes;
    target.dataFailureCount += source.dataFailureCount;
    target.finalDataFailureCount += source.finalDataFailureCount;
    for (const auto& [reason, count] : source.mpduDropsByReason)
    {
        target.mpduDropsByReason[reason] += count;
    }
}

void
MergeMac(MacDirectionAccumulator& target, const MacDirectionAccumulator& source)
{
    target.estimatedTransmitEventCount += source.estimatedTransmitEventCount;
    target.estimatedTransmittedTcpPayloadBytes += source.estimatedTransmittedTcpPayloadBytes;
    target.estimatedReceiveEventCount += source.estimatedReceiveEventCount;
    target.estimatedReceivedTcpPayloadBytes += source.estimatedReceivedTcpPayloadBytes;
    target.transmitDropCount += source.transmitDropCount;
    target.transmitDropPacketBytes += source.transmitDropPacketBytes;
    target.mpduDropCount += source.mpduDropCount;
    target.mpduDropBytes += source.mpduDropBytes;
    target.dataFailureCount += source.dataFailureCount;
    target.finalDataFailureCount += source.finalDataFailureCount;
    for (const auto& [reason, count] : source.mpduDropsByReason)
    {
        target.mpduDropsByReason[reason] += count;
    }
    for (const auto& [nodeId, peer] : source.peersByNodeId)
    {
        MergeMacPeer(target.peersByNodeId[nodeId], peer);
    }
}

void
MergePhyPeer(PhyPeerAccumulator& target, const PhyPeerAccumulator& source)
{
    target.taggedPayloadBytes += source.taggedPayloadBytes;
    target.uniqueTaggedPayloadBytes += source.uniqueTaggedPayloadBytes;
    target.transmissionAttemptCount += source.transmissionAttemptCount;
    target.retransmissionCount += source.retransmissionCount;
    target.dataRateBpsUs += source.dataRateBpsUs;
    target.transmissionAirtimeUs += source.transmissionAirtimeUs;
}

void
MergePhy(PhyDirectionAccumulator& target, const PhyDirectionAccumulator& source)
{
    MergePhyPeer(target, source);
    target.taggedMpduCount += source.taggedMpduCount;
    target.completeTaggedMpduBytes += source.completeTaggedMpduBytes;
    for (const auto& [nodeId, peer] : source.peersByNodeId)
    {
        MergePhyPeer(target.peersByNodeId[nodeId], peer);
    }
}

void
MergeTcp(TcpWindowAccumulator& target, const TcpWindowAccumulator& source)
{
    target.congestionWindowBytesUs += source.congestionWindowBytesUs;
    target.congestionWindowObservationDurationUs += source.congestionWindowObservationDurationUs;
    if (source.lastCongestionWindowBytes)
    {
        target.lastCongestionWindowBytes = source.lastCongestionWindowBytes;
    }
    target.roundTripTimeUs.Merge(source.roundTripTimeUs);
}

bool
HasAppPeer(const AppPeerAccumulator& value)
{
    return value.acceptedSendCount || value.acceptedPayloadBytes || value.receiveEventCount ||
           value.receivedPayloadBytes || value.dropEventCount || value.droppedPayloadBytes;
}

bool
HasMacPeer(const MacPeerAccumulator& value)
{
    return value.estimatedTransmitEventCount || value.estimatedTransmittedTcpPayloadBytes ||
           value.estimatedReceiveEventCount || value.estimatedReceivedTcpPayloadBytes ||
           value.mpduDropCount || value.mpduDropBytes || value.dataFailureCount ||
           value.finalDataFailureCount || !value.mpduDropsByReason.empty();
}

bool
HasPhyPeer(const PhyPeerAccumulator& value)
{
    return value.taggedPayloadBytes || value.uniqueTaggedPayloadBytes ||
           value.transmissionAttemptCount || value.retransmissionCount ||
           value.dataRateBpsUs != 0.0L || value.transmissionAirtimeUs != 0.0L;
}

} // namespace

void
SampleAccumulator::Add(double value)
{
    if (count == 0)
    {
        minimum = value;
        maximum = value;
    }
    else
    {
        minimum = std::min(minimum, value);
        maximum = std::max(maximum, value);
    }
    ++count;
    sum += value;
    sumSquares += static_cast<long double>(value) * value;
}

void
SampleAccumulator::Merge(const SampleAccumulator& other)
{
    if (other.count == 0)
    {
        return;
    }
    if (count == 0)
    {
        *this = other;
        return;
    }
    count += other.count;
    sum += other.sum;
    sumSquares += other.sumSquares;
    minimum = std::min(minimum, other.minimum);
    maximum = std::max(maximum, other.maximum);
}

void
MergeLocalEntityWindowAccumulator(LocalEntityWindowAccumulator& target,
                                  const LocalEntityWindowAccumulator& source)
{
    MergeApp(target.app.uplink, source.app.uplink);
    MergeApp(target.app.downlink, source.app.downlink);
    MergeDevice(target.deviceTransmission.uplink, source.deviceTransmission.uplink);
    MergeDevice(target.deviceTransmission.downlink, source.deviceTransmission.downlink);
    target.applicationToPhyDelayUs.uplink.Merge(source.applicationToPhyDelayUs.uplink);
    target.applicationToPhyDelayUs.downlink.Merge(source.applicationToPhyDelayUs.downlink);
    MergeMac(target.mac.uplink, source.mac.uplink);
    MergeMac(target.mac.downlink, source.mac.downlink);
    AddValue(target.phy.busyTimeUs, source.phy.busyTimeUs);
    MergePhy(target.phy.uplink, source.phy.uplink);
    MergePhy(target.phy.downlink, source.phy.downlink);
    for (const auto& [key, connection] : source.tcpConnections)
    {
        MergeTcp(target.tcpConnections[key], connection);
    }
}

bool
HasEntityActivity(const LocalEntityWindowAccumulator& value)
{
    const auto HasApp = [](const AppDirectionAccumulator& app) {
        if (app.acceptedSendCount || app.acceptedPayloadBytes || app.receiveEventCount ||
            app.receivedPayloadBytes || app.dropEventCount || app.droppedPayloadBytes ||
            app.receiveInterArrivalUs.count)
        {
            return true;
        }
        return std::ranges::any_of(app.agents,
                                   [](const auto& item) {
                                       const auto& agent = item.second;
                                       return agent.acceptedSendCount ||
                                              agent.acceptedPayloadBytes || agent.dropEventCount ||
                                              agent.droppedPayloadBytes;
                                   }) ||
               std::ranges::any_of(app.peersByNodeId,
                                   [](const auto& item) { return HasAppPeer(item.second); });
    };
    const auto HasDevice = [](const DeviceTransmissionAccumulator& device) {
        return device.estimatedTransmittedTcpPayloadBytes ||
               device.estimatedMatchedTcpPayloadBytes || device.matchedPacketCount ||
               device.transmissionDurationUs.count;
    };
    const auto HasMac = [](const MacDirectionAccumulator& mac) {
        return mac.estimatedTransmitEventCount || mac.estimatedTransmittedTcpPayloadBytes ||
               mac.estimatedReceiveEventCount || mac.estimatedReceivedTcpPayloadBytes ||
               mac.transmitDropCount || mac.transmitDropPacketBytes || mac.mpduDropCount ||
               mac.mpduDropBytes || mac.dataFailureCount || mac.finalDataFailureCount ||
               !mac.mpduDropsByReason.empty() ||
               std::ranges::any_of(mac.peersByNodeId,
                                   [](const auto& item) { return HasMacPeer(item.second); });
    };
    const auto HasPhy = [](const PhyDirectionAccumulator& phy) {
        return HasPhyPeer(phy) || phy.taggedMpduCount || phy.completeTaggedMpduBytes ||
               std::ranges::any_of(phy.peersByNodeId,
                                   [](const auto& item) { return HasPhyPeer(item.second); });
    };
    const bool tcp = std::ranges::any_of(value.tcpConnections, [](const auto& item) {
        const auto& connection = item.second;
        return connection.congestionWindowBytesUs != 0.0L ||
               connection.congestionWindowObservationDurationUs ||
               connection.lastCongestionWindowBytes || connection.roundTripTimeUs.count;
    });
    return HasApp(value.app.uplink) || HasApp(value.app.downlink) ||
           HasDevice(value.deviceTransmission.uplink) ||
           HasDevice(value.deviceTransmission.downlink) ||
           value.applicationToPhyDelayUs.uplink.count ||
           value.applicationToPhyDelayUs.downlink.count || HasMac(value.mac.uplink) ||
           HasMac(value.mac.downlink) || value.phy.busyTimeUs || HasPhy(value.phy.uplink) ||
           HasPhy(value.phy.downlink) || tcp;
}

void
ExperimentEntityRegistry::RegisterAccessPoint(uint32_t accessPointId,
                                              uint32_t nodeId,
                                              std::string nodeLabel,
                                              std::string ipv4)
{
    if (m_accessPointIndexes.contains(accessPointId))
    {
        throw std::invalid_argument("Duplicate access point identity");
    }
    if (m_nodeLocations.contains(nodeId))
    {
        throw std::invalid_argument("Duplicate node ID");
    }
    if (m_ipv4Locations.contains(ipv4))
    {
        throw std::invalid_argument("Duplicate IPv4 address");
    }

    m_accessPoints.push_back({ExperimentEntityKind::ACCESS_POINT,
                              accessPointId,
                              std::nullopt,
                              nodeId,
                              std::move(nodeLabel),
                              std::move(ipv4)});
    std::sort(m_accessPoints.begin(),
              m_accessPoints.end(),
              [](const auto& left, const auto& right) {
                  return left.accessPointId < right.accessPointId;
              });
    RebuildLookupIndexes();
}

void
ExperimentEntityRegistry::RegisterStation(uint32_t accessPointId,
                                          uint32_t stationIndex,
                                          uint32_t nodeId,
                                          std::string nodeLabel,
                                          std::string ipv4)
{
    const auto identity = std::make_pair(accessPointId, stationIndex);
    if (m_stationIndexes.contains(identity))
    {
        throw std::invalid_argument("Duplicate station identity");
    }
    if (m_nodeLocations.contains(nodeId))
    {
        throw std::invalid_argument("Duplicate node ID");
    }
    if (m_ipv4Locations.contains(ipv4))
    {
        throw std::invalid_argument("Duplicate IPv4 address");
    }

    m_stations.push_back({ExperimentEntityKind::STATION,
                          accessPointId,
                          stationIndex,
                          nodeId,
                          std::move(nodeLabel),
                          std::move(ipv4)});
    std::sort(m_stations.begin(), m_stations.end(), [](const auto& left, const auto& right) {
        return std::tie(left.accessPointId, left.stationIndex) <
               std::tie(right.accessPointId, right.stationIndex);
    });
    RebuildLookupIndexes();
}

const ExperimentEntityIdentity*
ExperimentEntityRegistry::FindByNodeId(uint32_t nodeId) const
{
    const auto iterator = m_nodeLocations.find(nodeId);
    if (iterator == m_nodeLocations.end())
    {
        return nullptr;
    }
    return iterator->second.isAccessPoint ? &m_accessPoints.at(iterator->second.index)
                                          : &m_stations.at(iterator->second.index);
}

const ExperimentEntityIdentity*
ExperimentEntityRegistry::FindByIpv4(std::string_view ipv4) const
{
    const auto iterator = m_ipv4Locations.find(ipv4);
    if (iterator == m_ipv4Locations.end())
    {
        return nullptr;
    }
    return iterator->second.isAccessPoint ? &m_accessPoints.at(iterator->second.index)
                                          : &m_stations.at(iterator->second.index);
}

const std::vector<ExperimentEntityIdentity>&
ExperimentEntityRegistry::GetAccessPoints() const
{
    return m_accessPoints;
}

const std::vector<ExperimentEntityIdentity>&
ExperimentEntityRegistry::GetStations() const
{
    return m_stations;
}

void
ExperimentEntityRegistry::RebuildLookupIndexes()
{
    m_accessPointIndexes.clear();
    m_stationIndexes.clear();
    m_nodeLocations.clear();
    m_ipv4Locations.clear();

    for (std::size_t index = 0; index < m_accessPoints.size(); ++index)
    {
        const auto& identity = m_accessPoints.at(index);
        m_accessPointIndexes.emplace(identity.accessPointId, index);
        const EntityLocation location{true, index};
        m_nodeLocations.emplace(identity.nodeId, location);
        m_ipv4Locations.emplace(identity.ipv4, location);
    }
    for (std::size_t index = 0; index < m_stations.size(); ++index)
    {
        const auto& identity = m_stations.at(index);
        m_stationIndexes.emplace(
            std::make_pair(identity.accessPointId, identity.stationIndex.value()),
            index);
        const EntityLocation location{false, index};
        m_nodeLocations.emplace(identity.nodeId, location);
        m_ipv4Locations.emplace(identity.ipv4, location);
    }
}

bool
ResolveExperimentWindow(int64_t relativeUs,
                        int64_t experimentDurationUs,
                        int64_t windowUs,
                        ExperimentWindowBounds& bounds)
{
    if (relativeUs < 0 || experimentDurationUs <= 0 || windowUs <= 0 ||
        relativeUs >= experimentDurationUs)
    {
        return false;
    }

    const int64_t startUs = relativeUs - relativeUs % windowUs;
    bounds = {static_cast<uint64_t>(relativeUs / windowUs),
              startUs,
              std::min(windowUs, experimentDurationUs - startUs)};
    return true;
}

std::vector<std::pair<uint64_t, int64_t>>
SplitExperimentInterval(int64_t relativeStartUs,
                        int64_t relativeEndUs,
                        int64_t experimentDurationUs,
                        int64_t windowUs)
{
    if (experimentDurationUs <= 0 || windowUs <= 0)
    {
        return {};
    }

    const int64_t clippedStartUs =
        std::min(std::max(relativeStartUs, int64_t{0}), experimentDurationUs);
    const int64_t clippedEndUs =
        std::min(std::max(relativeEndUs, int64_t{0}), experimentDurationUs);
    if (clippedStartUs >= clippedEndUs)
    {
        return {};
    }

    std::vector<std::pair<uint64_t, int64_t>> pieces;
    int64_t currentUs = clippedStartUs;
    while (currentUs < clippedEndUs)
    {
        ExperimentWindowBounds bounds{};
        if (!ResolveExperimentWindow(currentUs, experimentDurationUs, windowUs, bounds))
        {
            return {};
        }
        const int64_t remainingWindowUs = bounds.durationUs - (currentUs - bounds.startUs);
        const int64_t overlapUs = std::min(remainingWindowUs, clippedEndUs - currentUs);
        pieces.emplace_back(bounds.index, overlapUs);
        currentUs += overlapUs;
    }
    return pieces;
}

} // namespace ns3
