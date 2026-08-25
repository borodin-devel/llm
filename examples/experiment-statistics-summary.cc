#include "experiment-statistics-internal.h"
#include "experiment-statistics.h"
#include "traffic-coordinator.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace ns3
{

namespace
{

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

template <typename Duration>
std::optional<double>
RateMbps(uint64_t bytes, Duration durationUs)
{
    if (durationUs <= 0)
    {
        return std::nullopt;
    }
    return static_cast<double>(static_cast<long double>(bytes) * 8.0L /
                               static_cast<long double>(durationUs));
}

std::optional<double>
SharePercent(uint64_t bytes, uint64_t totalBytes)
{
    if (totalBytes == 0)
    {
        return std::nullopt;
    }
    return static_cast<double>(bytes) * 100.0 / static_cast<double>(totalBytes);
}

SampleDistributionOutput
FinalizeDistribution(const SampleAccumulator& raw)
{
    SampleDistributionOutput output;
    output.sampleCount = raw.count;
    if (raw.count == 0)
    {
        return output;
    }
    const long double average = raw.sum / raw.count;
    const long double variance =
        std::max<long double>(0.0L, raw.sumSquares / raw.count - average * average);
    output.averageUs = static_cast<double>(average);
    output.standardDeviationUs = std::sqrt(static_cast<double>(variance));
    output.minimumUs = raw.minimum;
    output.maximumUs = raw.maximum;
    return output;
}

std::string
PeerIpv4(const ExperimentStatisticsState& statistics, uint32_t nodeId)
{
    const auto* peer = statistics.entityRegistry.FindByNodeId(nodeId);
    return peer ? peer->ipv4 : std::string{};
}

GeneralDirectionOutput
FinalizeGeneral(const DeviceTransmissionAccumulator& device, const SampleAccumulator& delay)
{
    GeneralDirectionOutput output;
    output.estimatedTransmittedTcpPayloadBytes = device.estimatedTransmittedTcpPayloadBytes;
    output.estimatedMatchedTcpPayloadBytes = device.estimatedMatchedTcpPayloadBytes;
    output.matchedPacketCount = device.matchedPacketCount;
    output.totalTransmissionDurationUs = static_cast<uint64_t>(device.transmissionDurationUs.sum);
    const auto duration = FinalizeDistribution(device.transmissionDurationUs);
    output.averageTransmissionDurationUs = duration.averageUs;
    output.transmissionDurationStandardDeviationUs = duration.standardDeviationUs;
    output.minimumTransmissionDurationUs = duration.minimumUs;
    output.maximumTransmissionDurationUs = duration.maximumUs;
    output.effectiveThroughputMbps =
        RateMbps(device.estimatedMatchedTcpPayloadBytes, output.totalTransmissionDurationUs);
    output.applicationToPhyDelay = FinalizeDistribution(delay);
    return output;
}

AppDirectionOutput
FinalizeApp(const AppDirectionAccumulator& raw,
            int64_t durationUs,
            const ExperimentStatisticsState& statistics)
{
    AppDirectionOutput output;
    output.acceptedSendCount = raw.acceptedSendCount;
    output.acceptedPayloadBytes = raw.acceptedPayloadBytes;
    output.acceptedThroughputMbps = RateMbps(raw.acceptedPayloadBytes, durationUs);
    output.receiveEventCount = raw.receiveEventCount;
    output.receivedPayloadBytes = raw.receivedPayloadBytes;
    output.receivedThroughputMbps = RateMbps(raw.receivedPayloadBytes, durationUs);
    output.dropEventCount = raw.dropEventCount;
    output.droppedPayloadBytes = raw.droppedPayloadBytes;
    output.receiveInterArrivalTime = FinalizeDistribution(raw.receiveInterArrivalUs);
    for (const auto& [key, value] : raw.agents)
    {
        if (!(value.acceptedSendCount || value.acceptedPayloadBytes || value.dropEventCount ||
              value.droppedPayloadBytes))
        {
            continue;
        }
        output.agents.push_back({key,
                                 value.acceptedSendCount,
                                 value.acceptedPayloadBytes,
                                 RateMbps(value.acceptedPayloadBytes, durationUs),
                                 SharePercent(value.acceptedPayloadBytes, raw.acceptedPayloadBytes),
                                 value.dropEventCount,
                                 value.droppedPayloadBytes});
    }
    for (const auto& [nodeId, value] : raw.peersByNodeId)
    {
        if (!HasAppPeer(value))
        {
            continue;
        }
        output.peers.push_back({nodeId,
                                PeerIpv4(statistics, nodeId),
                                value.acceptedSendCount,
                                value.acceptedPayloadBytes,
                                RateMbps(value.acceptedPayloadBytes, durationUs),
                                SharePercent(value.acceptedPayloadBytes, raw.acceptedPayloadBytes),
                                value.receiveEventCount,
                                value.receivedPayloadBytes,
                                RateMbps(value.receivedPayloadBytes, durationUs),
                                SharePercent(value.receivedPayloadBytes, raw.receivedPayloadBytes),
                                value.dropEventCount,
                                value.droppedPayloadBytes});
    }
    return output;
}

TcpDirectionOutput
FinalizeTcp(const LocalEntityWindowAccumulator& raw,
            ExperimentDirection direction,
            const ExperimentStatisticsState& statistics)
{
    TcpDirectionOutput output;
    for (const auto& [key, value] : raw.tcpConnections)
    {
        if (key.first != direction)
        {
            continue;
        }
        TcpConnectionOutput connection;
        connection.peerNodeId = key.second;
        connection.peerIpv4 = PeerIpv4(statistics, key.second);
        connection.congestionWindowObservationDurationUs =
            value.congestionWindowObservationDurationUs;
        if (value.congestionWindowObservationDurationUs > 0)
        {
            connection.averageCongestionWindowBytes = static_cast<double>(
                value.congestionWindowBytesUs / value.congestionWindowObservationDurationUs);
        }
        if (value.lastCongestionWindowBytes)
        {
            connection.lastCongestionWindowBytes = *value.lastCongestionWindowBytes;
        }
        connection.roundTripTime = FinalizeDistribution(value.roundTripTimeUs);
        output.connections.push_back(std::move(connection));
    }
    return output;
}

std::vector<MacDropReasonOutput>
FinalizeReasons(const std::map<int, uint64_t>& reasons)
{
    std::vector<MacDropReasonOutput> output;
    for (const auto& [reason, count] : reasons)
    {
        if (count > 0)
        {
            output.push_back({reason, count});
        }
    }
    return output;
}

MacDirectionOutput
FinalizeMac(const MacDirectionAccumulator& raw,
            int64_t durationUs,
            const ExperimentStatisticsState& statistics)
{
    MacDirectionOutput output;
    output.estimatedTransmitEventCount = raw.estimatedTransmitEventCount;
    output.estimatedTransmittedTcpPayloadBytes = raw.estimatedTransmittedTcpPayloadBytes;
    output.estimatedTransmitThroughputMbps =
        RateMbps(raw.estimatedTransmittedTcpPayloadBytes, durationUs);
    output.estimatedReceiveEventCount = raw.estimatedReceiveEventCount;
    output.estimatedReceivedTcpPayloadBytes = raw.estimatedReceivedTcpPayloadBytes;
    output.estimatedReceiveThroughputMbps =
        RateMbps(raw.estimatedReceivedTcpPayloadBytes, durationUs);
    output.transmitDropCount = raw.transmitDropCount;
    output.transmitDropPacketBytes = raw.transmitDropPacketBytes;
    output.mpduDropCount = raw.mpduDropCount;
    output.mpduDropBytes = raw.mpduDropBytes;
    output.dataFailureCount = raw.dataFailureCount;
    output.finalDataFailureCount = raw.finalDataFailureCount;
    output.mpduDropsByReason = FinalizeReasons(raw.mpduDropsByReason);
    for (const auto& [nodeId, value] : raw.peersByNodeId)
    {
        if (!HasMacPeer(value))
        {
            continue;
        }
        output.peers.push_back({nodeId,
                                PeerIpv4(statistics, nodeId),
                                value.estimatedTransmitEventCount,
                                value.estimatedTransmittedTcpPayloadBytes,
                                RateMbps(value.estimatedTransmittedTcpPayloadBytes, durationUs),
                                value.estimatedReceiveEventCount,
                                value.estimatedReceivedTcpPayloadBytes,
                                RateMbps(value.estimatedReceivedTcpPayloadBytes, durationUs),
                                value.mpduDropCount,
                                value.mpduDropBytes,
                                value.dataFailureCount,
                                value.finalDataFailureCount,
                                FinalizeReasons(value.mpduDropsByReason)});
    }
    return output;
}

template <typename Output>
void
FinalizePhyBase(Output& output, const PhyPeerAccumulator& raw, int64_t durationUs)
{
    output.taggedPayloadBytes = raw.taggedPayloadBytes;
    output.uniqueTaggedPayloadBytes = raw.uniqueTaggedPayloadBytes;
    output.transmissionAttemptCount = raw.transmissionAttemptCount;
    output.retransmissionCount = raw.retransmissionCount;
    output.transmissionAirtimeUs = static_cast<double>(raw.transmissionAirtimeUs);
    output.averageDataRateMbps = CalculateAveragePhyDataRateMbps(raw);
    output.throughputMbps = RateMbps(raw.taggedPayloadBytes, durationUs);
}

PhyDirectionOutput
FinalizePhy(const PhyDirectionAccumulator& raw,
            int64_t durationUs,
            const ExperimentStatisticsState& statistics)
{
    PhyDirectionOutput output;
    FinalizePhyBase(output, raw, durationUs);
    output.taggedMpduCount = raw.taggedMpduCount;
    output.completeTaggedMpduBytes = raw.completeTaggedMpduBytes;
    for (const auto& [nodeId, value] : raw.peersByNodeId)
    {
        if (!HasPhyPeer(value))
        {
            continue;
        }
        PhyPeerOutput peer;
        peer.peerNodeId = nodeId;
        peer.peerIpv4 = PeerIpv4(statistics, nodeId);
        FinalizePhyBase(peer, value, durationUs);
        output.peers.push_back(std::move(peer));
    }
    return output;
}

int64_t
ExperimentDurationUs(const ExperimentStatisticsState& statistics)
{
    return ConvertExperimentDurationMsToUs(statistics.coordinator.GetMaxExperimentDurationMs());
}

AccessPointStatisticsOutput
FinalizeAccessPoint(const ExperimentEntityIdentity& identity,
                    const LocalEntityWindowAccumulator& raw,
                    int64_t durationUs,
                    const ExperimentStatisticsState& statistics)
{
    return {identity.accessPointId,
            identity.nodeId,
            identity.nodeLabel,
            identity.ipv4,
            FinalizeEntityStatistics(raw, durationUs, statistics)};
}

StationStatisticsOutput
FinalizeStation(const ExperimentEntityIdentity& identity,
                const LocalEntityWindowAccumulator& raw,
                int64_t durationUs,
                const ExperimentStatisticsState& statistics)
{
    return {identity.accessPointId,
            identity.stationIndex.value(),
            identity.nodeId,
            identity.nodeLabel,
            identity.ipv4,
            FinalizeEntityStatistics(raw, durationUs, statistics)};
}

} // namespace

void
MergeStationIntoAccessPoint(LocalEntityWindowAccumulator& target,
                            const LocalEntityWindowAccumulator& station,
                            uint32_t accessPointNodeId,
                            uint32_t stationNodeId)
{
    target.deviceTransmission.uplink.estimatedTransmittedTcpPayloadBytes +=
        station.deviceTransmission.uplink.estimatedTransmittedTcpPayloadBytes;
    target.deviceTransmission.uplink.estimatedMatchedTcpPayloadBytes +=
        station.deviceTransmission.uplink.estimatedMatchedTcpPayloadBytes;
    target.deviceTransmission.uplink.matchedPacketCount +=
        station.deviceTransmission.uplink.matchedPacketCount;
    target.deviceTransmission.uplink.transmissionDurationUs.Merge(
        station.deviceTransmission.uplink.transmissionDurationUs);
    target.applicationToPhyDelayUs.uplink.Merge(station.applicationToPhyDelayUs.uplink);

    auto& uplinkApp = target.app.uplink;
    uplinkApp.acceptedSendCount += station.app.uplink.acceptedSendCount;
    uplinkApp.acceptedPayloadBytes += station.app.uplink.acceptedPayloadBytes;
    uplinkApp.dropEventCount += station.app.uplink.dropEventCount;
    uplinkApp.droppedPayloadBytes += station.app.uplink.droppedPayloadBytes;
    for (const auto& [key, agent] : station.app.uplink.agents)
    {
        auto& parentAgent = uplinkApp.agents[key];
        parentAgent.acceptedSendCount += agent.acceptedSendCount;
        parentAgent.acceptedPayloadBytes += agent.acceptedPayloadBytes;
        parentAgent.dropEventCount += agent.dropEventCount;
        parentAgent.droppedPayloadBytes += agent.droppedPayloadBytes;
    }
    if (const auto peer = station.app.uplink.peersByNodeId.find(accessPointNodeId);
        peer != station.app.uplink.peersByNodeId.end())
    {
        auto& parentPeer = uplinkApp.peersByNodeId[stationNodeId];
        parentPeer.acceptedSendCount += peer->second.acceptedSendCount;
        parentPeer.acceptedPayloadBytes += peer->second.acceptedPayloadBytes;
        parentPeer.dropEventCount += peer->second.dropEventCount;
        parentPeer.droppedPayloadBytes += peer->second.droppedPayloadBytes;
    }

    auto& downlinkApp = target.app.downlink;
    downlinkApp.receiveEventCount += station.app.downlink.receiveEventCount;
    downlinkApp.receivedPayloadBytes += station.app.downlink.receivedPayloadBytes;
    downlinkApp.receiveInterArrivalUs.Merge(station.app.downlink.receiveInterArrivalUs);
    if (const auto peer = station.app.downlink.peersByNodeId.find(accessPointNodeId);
        peer != station.app.downlink.peersByNodeId.end())
    {
        auto& parentPeer = downlinkApp.peersByNodeId[stationNodeId];
        parentPeer.receiveEventCount += peer->second.receiveEventCount;
        parentPeer.receivedPayloadBytes += peer->second.receivedPayloadBytes;
    }

    for (const auto& [key, connection] : station.tcpConnections)
    {
        if (key.first == ExperimentDirection::UPLINK)
        {
            auto& parentConnection =
                target.tcpConnections[{ExperimentDirection::UPLINK, stationNodeId}];
            parentConnection.congestionWindowBytesUs += connection.congestionWindowBytesUs;
            parentConnection.congestionWindowObservationDurationUs +=
                connection.congestionWindowObservationDurationUs;
            if (connection.lastCongestionWindowBytes)
            {
                parentConnection.lastCongestionWindowBytes = connection.lastCongestionWindowBytes;
            }
            parentConnection.roundTripTimeUs.Merge(connection.roundTripTimeUs);
        }
    }

    auto& uplinkMac = target.mac.uplink;
    const auto& stationUplinkMac = station.mac.uplink;
    uplinkMac.estimatedTransmitEventCount += stationUplinkMac.estimatedTransmitEventCount;
    uplinkMac.estimatedTransmittedTcpPayloadBytes +=
        stationUplinkMac.estimatedTransmittedTcpPayloadBytes;
    uplinkMac.transmitDropCount += stationUplinkMac.transmitDropCount;
    uplinkMac.transmitDropPacketBytes += stationUplinkMac.transmitDropPacketBytes;
    uplinkMac.mpduDropCount += stationUplinkMac.mpduDropCount;
    uplinkMac.mpduDropBytes += stationUplinkMac.mpduDropBytes;
    uplinkMac.dataFailureCount += stationUplinkMac.dataFailureCount;
    uplinkMac.finalDataFailureCount += stationUplinkMac.finalDataFailureCount;
    for (const auto& [reason, count] : stationUplinkMac.mpduDropsByReason)
    {
        uplinkMac.mpduDropsByReason[reason] += count;
    }
    if (const auto peer = stationUplinkMac.peersByNodeId.find(accessPointNodeId);
        peer != stationUplinkMac.peersByNodeId.end())
    {
        auto& parentPeer = uplinkMac.peersByNodeId[stationNodeId];
        parentPeer.estimatedTransmitEventCount += peer->second.estimatedTransmitEventCount;
        parentPeer.estimatedTransmittedTcpPayloadBytes +=
            peer->second.estimatedTransmittedTcpPayloadBytes;
        parentPeer.mpduDropCount += peer->second.mpduDropCount;
        parentPeer.mpduDropBytes += peer->second.mpduDropBytes;
        parentPeer.dataFailureCount += peer->second.dataFailureCount;
        parentPeer.finalDataFailureCount += peer->second.finalDataFailureCount;
        for (const auto& [reason, count] : peer->second.mpduDropsByReason)
        {
            parentPeer.mpduDropsByReason[reason] += count;
        }
    }

    auto& downlinkMac = target.mac.downlink;
    const auto& stationDownlinkMac = station.mac.downlink;
    downlinkMac.estimatedReceiveEventCount += stationDownlinkMac.estimatedReceiveEventCount;
    downlinkMac.estimatedReceivedTcpPayloadBytes +=
        stationDownlinkMac.estimatedReceivedTcpPayloadBytes;
    if (const auto peer = stationDownlinkMac.peersByNodeId.find(accessPointNodeId);
        peer != stationDownlinkMac.peersByNodeId.end())
    {
        auto& parentPeer = downlinkMac.peersByNodeId[stationNodeId];
        parentPeer.estimatedReceiveEventCount += peer->second.estimatedReceiveEventCount;
        parentPeer.estimatedReceivedTcpPayloadBytes +=
            peer->second.estimatedReceivedTcpPayloadBytes;
    }
}

UnifiedSummaryRawState
BuildUnifiedSummaryRawState(const ExperimentStatisticsState& statistics)
{
    UnifiedSummaryRawState raw;
    raw.localWindows = statistics.unifiedWindows;
    for (const auto& identity : statistics.entityRegistry.GetAccessPoints())
    {
        raw.accessPointOverall[identity.nodeId];
    }
    for (const auto& identity : statistics.entityRegistry.GetStations())
    {
        raw.stationOverall[identity.nodeId];
    }

    for (const auto& [windowIndex, localEntities] : raw.localWindows)
    {
        for (const auto& accessPoint : statistics.entityRegistry.GetAccessPoints())
        {
            auto& parent = raw.accessPointWindows[windowIndex][accessPoint.nodeId];
            if (const auto local = localEntities.find(accessPoint.nodeId);
                local != localEntities.end())
            {
                parent = local->second;
            }
            for (const auto& station : statistics.entityRegistry.GetStations())
            {
                if (station.accessPointId != accessPoint.accessPointId)
                {
                    continue;
                }
                if (const auto child = localEntities.find(station.nodeId);
                    child != localEntities.end())
                {
                    MergeStationIntoAccessPoint(parent,
                                                child->second,
                                                accessPoint.nodeId,
                                                station.nodeId);
                }
            }
            MergeLocalEntityWindowAccumulator(raw.accessPointOverall[accessPoint.nodeId], parent);
        }
        for (const auto& station : statistics.entityRegistry.GetStations())
        {
            if (const auto local = localEntities.find(station.nodeId); local != localEntities.end())
            {
                raw.stationWindows[windowIndex][station.nodeId] = local->second;
                MergeLocalEntityWindowAccumulator(raw.stationOverall[station.nodeId],
                                                  local->second);
            }
        }
    }
    return raw;
}

EntityStatisticsOutput
FinalizeEntityStatistics(const LocalEntityWindowAccumulator& raw,
                         int64_t durationUs,
                         const ExperimentStatisticsState& statistics)
{
    EntityStatisticsOutput output;
    output.generalStats.uplink =
        FinalizeGeneral(raw.deviceTransmission.uplink, raw.applicationToPhyDelayUs.uplink);
    output.generalStats.downlink =
        FinalizeGeneral(raw.deviceTransmission.downlink, raw.applicationToPhyDelayUs.downlink);
    output.appStats.uplink = FinalizeApp(raw.app.uplink, durationUs, statistics);
    output.appStats.downlink = FinalizeApp(raw.app.downlink, durationUs, statistics);
    output.tcpStats.uplink = FinalizeTcp(raw, ExperimentDirection::UPLINK, statistics);
    output.tcpStats.downlink = FinalizeTcp(raw, ExperimentDirection::DOWNLINK, statistics);
    output.macStats.uplink = FinalizeMac(raw.mac.uplink, durationUs, statistics);
    output.macStats.downlink = FinalizeMac(raw.mac.downlink, durationUs, statistics);
    output.phyStats.busyTimeUs = static_cast<uint64_t>(std::max<int64_t>(0, raw.phy.busyTimeUs));
    if (durationUs > 0)
    {
        output.phyStats.channelUtilizationPercent =
            std::min(100.0, static_cast<double>(output.phyStats.busyTimeUs) * 100.0 / durationUs);
    }
    output.phyStats.uplink = FinalizePhy(raw.phy.uplink, durationUs, statistics);
    output.phyStats.downlink = FinalizePhy(raw.phy.downlink, durationUs, statistics);
    return output;
}

UnifiedExperimentSummary
ExperimentStatistics::BuildUnifiedExperimentSummary()
{
    Finalize();
    const UnifiedSummaryRawState raw = BuildUnifiedSummaryRawState(*m_state);
    const int64_t experimentDurationUs = ExperimentDurationUs(*m_state);

    UnifiedExperimentSummary output;
    output.statisticsWindowMs = m_state->windowMs;
    output.accessPointInventory = m_state->entityRegistry.GetAccessPoints();
    output.stationInventory = m_state->entityRegistry.GetStations();
    const uint64_t windowCount =
        experimentDurationUs > 0
            ? static_cast<uint64_t>(experimentDurationUs / m_state->windowUs +
                                    (experimentDurationUs % m_state->windowUs != 0))
            : 0;
    for (const auto& [windowIndex, accessPoints] : raw.accessPointWindows)
    {
        if (windowIndex >= windowCount)
        {
            continue;
        }
        const int64_t startUs = static_cast<int64_t>(windowIndex) * m_state->windowUs;
        const int64_t durationUs = std::min(m_state->windowUs, experimentDurationUs - startUs);
        ExperimentWindowOutput window;
        window.windowIndex = windowIndex;
        window.windowStartMs = static_cast<double>(startUs) / 1000.0;
        window.windowDurationMs = static_cast<double>(durationUs) / 1000.0;
        for (const auto& identity : output.accessPointInventory)
        {
            const auto iterator = accessPoints.find(identity.nodeId);
            if (iterator != accessPoints.end() && HasEntityActivity(iterator->second))
            {
                window.accessPoints.push_back(
                    FinalizeAccessPoint(identity, iterator->second, durationUs, *m_state));
            }
        }
        const auto stationWindow = raw.stationWindows.find(windowIndex);
        for (const auto& identity : output.stationInventory)
        {
            if (stationWindow == raw.stationWindows.end())
            {
                break;
            }
            const auto iterator = stationWindow->second.find(identity.nodeId);
            if (iterator != stationWindow->second.end() && HasEntityActivity(iterator->second))
            {
                window.stations.push_back(
                    FinalizeStation(identity, iterator->second, durationUs, *m_state));
            }
        }
        if (!window.accessPoints.empty() || !window.stations.empty())
        {
            output.windows.push_back(std::move(window));
        }
    }

    for (const auto& identity : output.accessPointInventory)
    {
        output.overall.accessPoints.push_back(
            FinalizeAccessPoint(identity,
                                raw.accessPointOverall.at(identity.nodeId),
                                experimentDurationUs,
                                *m_state));
    }
    for (const auto& identity : output.stationInventory)
    {
        output.overall.stations.push_back(FinalizeStation(identity,
                                                          raw.stationOverall.at(identity.nodeId),
                                                          experimentDurationUs,
                                                          *m_state));
    }
    output.validation = ValidateUnifiedSummaryRawState(m_state->entityRegistry, raw);
    return output;
}

} // namespace ns3
