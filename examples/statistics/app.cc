#include "../runtime/traffic-coordinator.h"
#include "experiment-statistics.h"
#include "internal.h"

#include "ns3/abort.h"
#include "ns3/ap-generator.h"
#include "ns3/inet-socket-address.h"
#include "ns3/simulator.h"
#include "ns3/sta-llm-generator.h"
#include "ns3/traffic-sink.h"

#include <cmath>
#include <optional>
#include <sstream>
#include <tuple>
#include <utility>

namespace ns3
{

bool
AppReceiveStreamKey::operator<(const AppReceiveStreamKey& other) const
{
    return std::tie(nodeId, direction, peerNodeId) <
           std::tie(other.nodeId, other.direction, other.peerNodeId);
}

namespace
{

std::string
Ipv4ToString(Ipv4Address address)
{
    std::ostringstream stream;
    address.Print(stream);
    return stream.str();
}

std::optional<uint32_t>
ResolvePeerNodeId(const ExperimentStatisticsState& statistics, const Address& address)
{
    if (!InetSocketAddress::IsMatchingType(address))
    {
        return std::nullopt;
    }

    const auto* identity = statistics.entityRegistry.FindByIpv4(
        Ipv4ToString(InetSocketAddress::ConvertFrom(address).GetIpv4()));
    return identity ? std::optional<uint32_t>{identity->nodeId} : std::nullopt;
}

std::optional<uint32_t>
ResolveParentAccessPointNodeId(const ExperimentStatisticsState& statistics, uint32_t stationNodeId)
{
    const auto* station = statistics.entityRegistry.FindByNodeId(stationNodeId);
    if (!station || station->kind != ExperimentEntityKind::STATION)
    {
        return std::nullopt;
    }

    for (const auto& accessPoint : statistics.entityRegistry.GetAccessPoints())
    {
        if (accessPoint.accessPointId == station->accessPointId)
        {
            return accessPoint.nodeId;
        }
    }
    return std::nullopt;
}

AppDirectionAccumulator*
GetApplicationAccumulator(ExperimentStatisticsState& statistics,
                          uint32_t nodeId,
                          ExperimentDirection direction,
                          int64_t absoluteTimeUs)
{
    ExperimentWindowBounds bounds;
    if (!ResolveStatisticsEventWindow(statistics, absoluteTimeUs, bounds))
    {
        return nullptr;
    }
    return &statistics.unifiedWindows[bounds.index][nodeId].app.Get(direction);
}

} // namespace

void
ExperimentStatistics::RecordAcceptedApplicationSend(uint32_t nodeId,
                                                    ExperimentDirection direction,
                                                    std::optional<uint32_t> peerNodeId,
                                                    const std::string& agentKey,
                                                    uint32_t acceptedBytes,
                                                    int64_t absoluteTimeUs)
{
    auto* directionAccumulator =
        GetApplicationAccumulator(*m_state, nodeId, direction, absoluteTimeUs);
    if (!directionAccumulator)
    {
        return;
    }

    ++directionAccumulator->acceptedSendCount;
    directionAccumulator->acceptedPayloadBytes += acceptedBytes;
    auto& agent = directionAccumulator->agents[agentKey];
    ++agent.acceptedSendCount;
    agent.acceptedPayloadBytes += acceptedBytes;
    if (peerNodeId)
    {
        auto& peer = directionAccumulator->peersByNodeId[*peerNodeId];
        ++peer.acceptedSendCount;
        peer.acceptedPayloadBytes += acceptedBytes;
    }
}

void
ExperimentStatistics::RecordApplicationDrop(uint32_t nodeId,
                                            ExperimentDirection direction,
                                            std::optional<uint32_t> peerNodeId,
                                            const std::string& agentKey,
                                            uint32_t droppedBytes,
                                            int64_t absoluteTimeUs)
{
    auto* directionAccumulator =
        GetApplicationAccumulator(*m_state, nodeId, direction, absoluteTimeUs);
    if (!directionAccumulator)
    {
        return;
    }

    ++directionAccumulator->dropEventCount;
    directionAccumulator->droppedPayloadBytes += droppedBytes;
    auto& agent = directionAccumulator->agents[agentKey];
    ++agent.dropEventCount;
    agent.droppedPayloadBytes += droppedBytes;
    if (peerNodeId)
    {
        auto& peer = directionAccumulator->peersByNodeId[*peerNodeId];
        ++peer.dropEventCount;
        peer.droppedPayloadBytes += droppedBytes;
    }
}

void
ExperimentStatistics::RecordApplicationReceive(uint32_t nodeId,
                                               ExperimentDirection direction,
                                               std::optional<uint32_t> peerNodeId,
                                               uint32_t receivedBytes,
                                               int64_t absoluteTimeUs)
{
    auto* directionAccumulator =
        GetApplicationAccumulator(*m_state, nodeId, direction, absoluteTimeUs);
    if (!directionAccumulator)
    {
        return;
    }

    ++directionAccumulator->receiveEventCount;
    directionAccumulator->receivedPayloadBytes += receivedBytes;
    if (peerNodeId)
    {
        auto& peer = directionAccumulator->peersByNodeId[*peerNodeId];
        ++peer.receiveEventCount;
        peer.receivedPayloadBytes += receivedBytes;
    }

    const AppReceiveStreamKey stream{nodeId, direction, peerNodeId};
    const auto previous = m_state->lastApplicationReceiveTimeUs.find(stream);
    if (previous != m_state->lastApplicationReceiveTimeUs.end())
    {
        directionAccumulator->receiveInterArrivalUs.Add(
            static_cast<double>(absoluteTimeUs - previous->second));
    }
    m_state->lastApplicationReceiveTimeUs[stream] = absoluteTimeUs;
}

void
ExperimentStatistics::RecordApAcceptedApplicationSend(uint32_t nodeId,
                                                      Address station,
                                                      std::string agentKey,
                                                      uint32_t acceptedBytes,
                                                      Time transmitTime)
{
    RecordAcceptedApplicationSend(nodeId,
                                  ExperimentDirection::DOWNLINK,
                                  ResolvePeerNodeId(*m_state, station),
                                  agentKey,
                                  acceptedBytes,
                                  transmitTime.GetMicroSeconds());
}

void
ExperimentStatistics::RecordApApplicationDrop(uint32_t nodeId,
                                              Address station,
                                              std::string agentKey,
                                              uint32_t droppedBytes,
                                              Time transmitTime)
{
    RecordApplicationDrop(nodeId,
                          ExperimentDirection::DOWNLINK,
                          ResolvePeerNodeId(*m_state, station),
                          agentKey,
                          droppedBytes,
                          transmitTime.GetMicroSeconds());
}

void
ExperimentStatistics::RecordStaAcceptedApplicationSend(uint32_t nodeId,
                                                       std::optional<uint32_t> peerNodeId,
                                                       std::string agentKey,
                                                       uint32_t acceptedBytes,
                                                       Time transmitTime)
{
    RecordAcceptedApplicationSend(nodeId,
                                  ExperimentDirection::UPLINK,
                                  peerNodeId,
                                  agentKey,
                                  acceptedBytes,
                                  transmitTime.GetMicroSeconds());
}

void
ExperimentStatistics::RecordStaApplicationDrop(uint32_t nodeId,
                                               std::optional<uint32_t> peerNodeId,
                                               std::string agentKey,
                                               uint32_t droppedBytes,
                                               Time transmitTime)
{
    RecordApplicationDrop(nodeId,
                          ExperimentDirection::UPLINK,
                          peerNodeId,
                          agentKey,
                          droppedBytes,
                          transmitTime.GetMicroSeconds());
}

void
ExperimentStatistics::RecordTrafficSinkReceive(uint32_t nodeId,
                                               ExperimentDirection direction,
                                               uint64_t receivedBytes,
                                               Address remoteAddress)
{
    RecordApplicationReceive(nodeId,
                             direction,
                             ResolvePeerNodeId(*m_state, remoteAddress),
                             static_cast<uint32_t>(receivedBytes),
                             Simulator::Now().GetMicroSeconds());
}

void
ExperimentStatistics::ConnectApGenerator(Ptr<APGenerator> generator, uint32_t nodeId)
{
    NS_ABORT_MSG_IF(
        !generator->TraceConnectWithoutContext(
            "Tx",
            MakeCallback(&ExperimentStatistics::RecordApAcceptedApplicationSend, this, nodeId)),
        "Failed to connect AP application transmit trace");
    NS_ABORT_MSG_IF(!generator->TraceConnectWithoutContext(
                        "AppTxDrop",
                        MakeCallback(&ExperimentStatistics::RecordApApplicationDrop, this, nodeId)),
                    "Failed to connect AP application drop trace");
    NS_ABORT_MSG_IF(
        !generator->TraceConnectWithoutContext(
            "CongestionWindowSample",
            MakeCallback(&ExperimentStatistics::RecordApCongestionWindow, this, nodeId)),
        "Failed to connect AP TCP congestion-window trace");
    NS_ABORT_MSG_IF(!generator->TraceConnectWithoutContext(
                        "RoundTripTimeSample",
                        MakeCallback(&ExperimentStatistics::RecordApRoundTripTime, this, nodeId)),
                    "Failed to connect AP TCP round-trip-time trace");
}

void
ExperimentStatistics::ConnectStaGenerator(Ptr<StaLlmGenerator> generator, uint32_t nodeId)
{
    const auto peerNodeId = ResolveParentAccessPointNodeId(*m_state, nodeId);
    NS_ABORT_MSG_IF(!generator->TraceConnectWithoutContext(
                        "TxCustom",
                        MakeCallback(&ExperimentStatistics::RecordStaAcceptedApplicationSend,
                                     this,
                                     nodeId,
                                     peerNodeId)),
                    "Failed to connect station application transmit trace");
    NS_ABORT_MSG_IF(!generator->TraceConnectWithoutContext(
                        "AppTxDrop",
                        MakeCallback(&ExperimentStatistics::RecordStaApplicationDrop,
                                     this,
                                     nodeId,
                                     peerNodeId)),
                    "Failed to connect station application drop trace");
    NS_ABORT_MSG_IF(
        !generator->TraceConnectWithoutContext(
            "CongestionWindowSample",
            MakeCallback(&ExperimentStatistics::RecordStaCongestionWindow, this, nodeId)),
        "Failed to connect station TCP congestion-window trace");
    NS_ABORT_MSG_IF(!generator->TraceConnectWithoutContext(
                        "RoundTripTimeSample",
                        MakeCallback(&ExperimentStatistics::RecordStaRoundTripTime, this, nodeId)),
                    "Failed to connect station TCP round-trip-time trace");
}

void
ExperimentStatistics::ConnectTrafficSink(Ptr<TrafficSink> sink,
                                         uint32_t nodeId,
                                         ExperimentDirection direction)
{
    NS_ABORT_MSG_IF(
        !sink->TraceConnectWithoutContext(
            "RxCustom",
            MakeCallback(&ExperimentStatistics::RecordTrafficSinkReceive, this, nodeId, direction)),
        "Failed to connect application sink receive trace");
}

} // namespace ns3
