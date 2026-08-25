#include "traffic-coordinator.h"
#include "wifi-statistics-internal.h"
#include "wifi-statistics.h"

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
ResolvePeerNodeId(const WifiStatisticsState& statistics, const Address& address)
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
ResolveParentAccessPointNodeId(const WifiStatisticsState& statistics, uint32_t stationNodeId)
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
GetApplicationAccumulator(WifiStatisticsState& statistics,
                          uint32_t nodeId,
                          ExperimentDirection direction,
                          int64_t absoluteTimeUs)
{
    uint64_t windowIndex = 0;
    if (!GetStatisticsWindowIndex(absoluteTimeUs,
                                  statistics.coordinator.GetExperimentStartUs(),
                                  statistics.coordinator.GetMaxExperimentDurationMs(),
                                  statistics.windowMs,
                                  windowIndex))
    {
        return nullptr;
    }
    return &statistics.unifiedWindows[windowIndex][nodeId].app.Get(direction);
}

NodeSecondStats*
GetLegacyApplicationAccumulator(WifiStatisticsState& statistics,
                                uint32_t nodeId,
                                int64_t absoluteTimeUs)
{
    const int64_t relativeUs = absoluteTimeUs - statistics.coordinator.GetExperimentStartUs();
    const int64_t durationUs = static_cast<int64_t>(
        std::ceil(statistics.coordinator.GetMaxExperimentDurationMs() * 1000.0));
    uint64_t secondIndex = 0;
    if (!GetNodeSecondIndex(relativeUs, durationUs, secondIndex))
    {
        return nullptr;
    }
    return &statistics.nodeSeconds[nodeId][secondIndex];
}

// Temporary compatibility write for the current cross-layer serializer.
// Task 7 removes this helper and the legacy node-second store.
void
RecordLegacyApplicationSend(WifiStatisticsState& statistics,
                            uint32_t nodeId,
                            const std::string& agentKey,
                            uint32_t bytes,
                            int64_t absoluteTimeUs,
                            bool dropped)
{
    auto* legacy = GetLegacyApplicationAccumulator(statistics, nodeId, absoluteTimeUs);
    if (!legacy)
    {
        return;
    }

    if (!dropped)
    {
        ++legacy->appTxEvents;
        legacy->appTxBytes += bytes;
        return;
    }

    ++legacy->appDropEvents;
    legacy->appDropBytes += bytes;
    auto& agent = legacy->appDropsByAgent[agentKey];
    ++agent.events;
    agent.bytes += bytes;
}

} // namespace

void
WifiStatistics::RecordAcceptedApplicationSend(uint32_t nodeId,
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

    RecordLegacyApplicationSend(*m_state, nodeId, agentKey, acceptedBytes, absoluteTimeUs, false);
}

void
WifiStatistics::RecordApplicationDrop(uint32_t nodeId,
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

    RecordLegacyApplicationSend(*m_state, nodeId, agentKey, droppedBytes, absoluteTimeUs, true);
}

void
WifiStatistics::RecordApplicationReceive(uint32_t nodeId,
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
WifiStatistics::RecordApAcceptedApplicationSend(uint32_t nodeId,
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
WifiStatistics::RecordApApplicationDrop(uint32_t nodeId,
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
WifiStatistics::RecordStaAcceptedApplicationSend(uint32_t nodeId,
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
WifiStatistics::RecordStaApplicationDrop(uint32_t nodeId,
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
WifiStatistics::RecordTrafficSinkReceive(uint32_t nodeId,
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
WifiStatistics::ConnectApGenerator(Ptr<APGenerator> generator, uint32_t nodeId)
{
    NS_ABORT_MSG_IF(
        !generator->TraceConnectWithoutContext(
            "Tx",
            MakeCallback(&WifiStatistics::RecordApAcceptedApplicationSend, this, nodeId)),
        "Failed to connect AP application transmit trace");
    NS_ABORT_MSG_IF(!generator->TraceConnectWithoutContext(
                        "AppTxDrop",
                        MakeCallback(&WifiStatistics::RecordApApplicationDrop, this, nodeId)),
                    "Failed to connect AP application drop trace");
}

void
WifiStatistics::ConnectStaGenerator(Ptr<StaLlmGenerator> generator, uint32_t nodeId)
{
    const auto peerNodeId = ResolveParentAccessPointNodeId(*m_state, nodeId);
    NS_ABORT_MSG_IF(!generator->TraceConnectWithoutContext(
                        "TxCustom",
                        MakeCallback(&WifiStatistics::RecordStaAcceptedApplicationSend,
                                     this,
                                     nodeId,
                                     peerNodeId)),
                    "Failed to connect station application transmit trace");
    NS_ABORT_MSG_IF(
        !generator->TraceConnectWithoutContext(
            "AppTxDrop",
            MakeCallback(&WifiStatistics::RecordStaApplicationDrop, this, nodeId, peerNodeId)),
        "Failed to connect station application drop trace");
}

void
WifiStatistics::ConnectTrafficSink(Ptr<TrafficSink> sink,
                                   uint32_t nodeId,
                                   ExperimentDirection direction)
{
    NS_ABORT_MSG_IF(
        !sink->TraceConnectWithoutContext(
            "RxCustom",
            MakeCallback(&WifiStatistics::RecordTrafficSinkReceive, this, nodeId, direction)),
        "Failed to connect application sink receive trace");
}

} // namespace ns3
