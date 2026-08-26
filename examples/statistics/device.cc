#include "../runtime/log.h"
#include "../runtime/traffic-coordinator.h"
#include "experiment-statistics.h"
#include "internal.h"

#include "ns3/config.h"
#include "ns3/iana-ieee802-numbers.h"
#include "ns3/iana-internet-protocol-numbers.h"
#include "ns3/ipv4-header.h"
#include "ns3/llc-snap-header.h"
#include "ns3/packet.h"
#include "ns3/simulator.h"
#include "ns3/tcp-header.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <sstream>
#include <tuple>
#include <utility>

namespace ns3
{

namespace
{

LogComponent& g_log = llm_example::GetScenarioLog();

std::string
Ipv4ToString(Ipv4Address address)
{
    std::ostringstream stream;
    address.Print(stream);
    return stream.str();
}

DeviceFlowKey
MakeDeviceFlowKey(const ParsedDeviceTcpPayload& payload)
{
    return {payload.sourceIpv4,
            payload.sourcePort,
            payload.destinationIpv4,
            payload.destinationPort,
            payload.estimatedPayloadBytes};
}

ExperimentDirection
GetTransmitDirection(const ExperimentEntityIdentity& entity)
{
    return entity.kind == ExperimentEntityKind::ACCESS_POINT ? ExperimentDirection::DOWNLINK
                                                             : ExperimentDirection::UPLINK;
}

ExperimentDirection
GetReceiveDirection(const ExperimentEntityIdentity& entity)
{
    return entity.kind == ExperimentEntityKind::ACCESS_POINT ? ExperimentDirection::UPLINK
                                                             : ExperimentDirection::DOWNLINK;
}

bool
ParseTcpPayload(Ptr<const Packet> packet, ParsedDeviceTcpPayload& parsed)
{
    const uint32_t packetSize = packet->GetSize();
    if (packetSize <= 60)
    {
        return false;
    }

    Ptr<Packet> packetCopy = packet->Copy();
    LlcSnapHeader llc;
    if (packetCopy->RemoveHeader(llc) == 0 || llc.GetType() != iana::ieee802numbers::IPV4)
    {
        return false;
    }

    Ipv4Header ipHeader;
    if (packetCopy->PeekHeader(ipHeader) == 0)
    {
        return false;
    }
    if (ipHeader.GetProtocol() != iana::internetprotocolnumbers::TCP)
    {
        return false;
    }
    parsed.sourceIpv4 = Ipv4ToString(ipHeader.GetSource());
    parsed.destinationIpv4 = Ipv4ToString(ipHeader.GetDestination());
    packetCopy->RemoveHeader(ipHeader);

    TcpHeader tcpHeader;
    if (packetCopy->PeekHeader(tcpHeader) == 0)
    {
        return false;
    }
    parsed.sourcePort = tcpHeader.GetSourcePort();
    parsed.destinationPort = tcpHeader.GetDestinationPort();
    parsed.estimatedPayloadBytes = packetSize - 60;
    return true;
}

void
DeviceTxTrace(ExperimentStatisticsState* statistics, std::string, Ptr<const Packet> packet)
{
    RecordDeviceTransmitPacket(*statistics, Simulator::Now().GetMicroSeconds(), packet);
}

void
DeviceRxTrace(ExperimentStatisticsState* statistics, std::string, Ptr<const Packet> packet)
{
    ParsedDeviceTcpPayload parsed;
    if (!ParseTcpPayload(packet, parsed))
    {
        return;
    }

    const int64_t nowUs = Simulator::Now().GetMicroSeconds();
    NS_LOG_INFO("RX [" << nowUs << " us] Payload: " << parsed.estimatedPayloadBytes
                       << " | tx: " << parsed.sourceIpv4 << ":" << parsed.sourcePort
                       << " -> rx: " << parsed.destinationIpv4 << ":" << parsed.destinationPort);
    RecordParsedDeviceReceive(*statistics, nowUs, parsed);
}

} // namespace

bool
DeviceFlowKey::operator<(const DeviceFlowKey& other) const
{
    return std::tie(sourceIpv4,
                    sourcePort,
                    destinationIpv4,
                    destinationPort,
                    estimatedPayloadBytes) < std::tie(other.sourceIpv4,
                                                      other.sourcePort,
                                                      other.destinationIpv4,
                                                      other.destinationPort,
                                                      other.estimatedPayloadBytes);
}

bool
ResolveStatisticsEventWindow(const ExperimentStatisticsState& statistics,
                             int64_t absoluteTimeUs,
                             ExperimentWindowBounds& bounds)
{
    const int64_t experimentStartUs = statistics.coordinator.GetExperimentStartUs();
    if (experimentStartUs < 0)
    {
        return false;
    }
    const int64_t experimentDurationUs =
        ConvertExperimentDurationMsToUs(statistics.coordinator.GetMaxExperimentDurationMs());
    return ResolveExperimentWindow(absoluteTimeUs - experimentStartUs,
                                   experimentDurationUs,
                                   statistics.windowUs,
                                   bounds);
}

void
RecordParsedDeviceTransmit(ExperimentStatisticsState& statistics,
                           int64_t absoluteTimeUs,
                           const ParsedDeviceTcpPayload& payload)
{
    ExperimentWindowBounds bounds;
    if (payload.estimatedPayloadBytes == 0 ||
        !ResolveStatisticsEventWindow(statistics, absoluteTimeUs, bounds))
    {
        return;
    }

    const auto* sender = statistics.entityRegistry.FindByIpv4(payload.sourceIpv4);
    if (!sender)
    {
        return;
    }
    const auto* receiver = statistics.entityRegistry.FindByIpv4(payload.destinationIpv4);
    const ExperimentDirection direction = GetTransmitDirection(*sender);
    auto& local = statistics.unifiedWindows[bounds.index][sender->nodeId];
    local.deviceTransmission.Get(direction).estimatedTransmittedTcpPayloadBytes +=
        payload.estimatedPayloadBytes;

    auto& mac = local.mac.Get(direction);
    ++mac.estimatedTransmitEventCount;
    mac.estimatedTransmittedTcpPayloadBytes += payload.estimatedPayloadBytes;
    if (receiver)
    {
        auto& peer = mac.peersByNodeId[receiver->nodeId];
        ++peer.estimatedTransmitEventCount;
        peer.estimatedTransmittedTcpPayloadBytes += payload.estimatedPayloadBytes;
    }

    statistics.deviceTransmitsByFlow[MakeDeviceFlowKey(payload)].push_back(
        {absoluteTimeUs, bounds.index, sender->nodeId, direction});
}

void
RecordParsedDeviceReceive(ExperimentStatisticsState& statistics,
                          int64_t absoluteTimeUs,
                          const ParsedDeviceTcpPayload& payload)
{
    ExperimentWindowBounds bounds;
    if (payload.estimatedPayloadBytes == 0 ||
        !ResolveStatisticsEventWindow(statistics, absoluteTimeUs, bounds))
    {
        return;
    }

    const auto* receiver = statistics.entityRegistry.FindByIpv4(payload.destinationIpv4);
    if (!receiver)
    {
        return;
    }
    const auto* sender = statistics.entityRegistry.FindByIpv4(payload.sourceIpv4);
    const ExperimentDirection direction = GetReceiveDirection(*receiver);
    auto& mac = statistics.unifiedWindows[bounds.index][receiver->nodeId].mac.Get(direction);
    ++mac.estimatedReceiveEventCount;
    mac.estimatedReceivedTcpPayloadBytes += payload.estimatedPayloadBytes;
    if (sender)
    {
        auto& peer = mac.peersByNodeId[sender->nodeId];
        ++peer.estimatedReceiveEventCount;
        peer.estimatedReceivedTcpPayloadBytes += payload.estimatedPayloadBytes;
    }

    statistics.deviceReceivesByFlow[MakeDeviceFlowKey(payload)].push_back(absoluteTimeUs);
}

bool
RecordDeviceTransmitPacket(ExperimentStatisticsState& statistics,
                           int64_t absoluteTimeUs,
                           Ptr<const Packet> packet)
{
    ParsedDeviceTcpPayload parsed;
    if (!ParseTcpPayload(packet, parsed))
    {
        return false;
    }
    NS_LOG_INFO("TX [" << absoluteTimeUs << " us] PayloadOnly: " << parsed.estimatedPayloadBytes
                       << " | tx: " << parsed.sourceIpv4 << ":" << parsed.sourcePort
                       << " -> rx: " << parsed.destinationIpv4 << ":" << parsed.destinationPort);
    RecordParsedDeviceTransmit(statistics, absoluteTimeUs, parsed);
    return true;
}

void
FinalizeDeviceStatistics(ExperimentStatisticsState& statistics)
{
    if (statistics.deviceStatisticsFinalized)
    {
        return;
    }
    statistics.deviceStatisticsFinalized = true;

    for (const auto& [flow, transmits] : statistics.deviceTransmitsByFlow)
    {
        const auto receiveIterator = statistics.deviceReceivesByFlow.find(flow);
        if (receiveIterator == statistics.deviceReceivesByFlow.end())
        {
            continue;
        }
        const auto& receives = receiveIterator->second;
        const std::size_t matchedCount = std::min(transmits.size(), receives.size());
        for (std::size_t index = 0; index < matchedCount; ++index)
        {
            const auto& transmit = transmits[index];
            if (receives[index] <= transmit.absoluteTimeUs)
            {
                continue;
            }
            auto& accumulator =
                statistics.unifiedWindows[transmit.windowIndex][transmit.senderNodeId]
                    .deviceTransmission.Get(transmit.direction);
            ++accumulator.matchedPacketCount;
            accumulator.estimatedMatchedTcpPayloadBytes += flow.estimatedPayloadBytes;
            accumulator.transmissionDurationUs.Add(
                static_cast<double>(receives[index] - transmit.absoluteTimeUs));
        }
    }
}

void
ExperimentStatistics::ConnectDeviceTraces()
{
    NS_ABORT_MSG_IF(m_state->deviceTracesConnected, "Wi-Fi device traces connected more than once");
    m_state->deviceTracesConnected = true;
    Config::Connect("/NodeList/*/DeviceList/*/$ns3::WifiNetDevice/Mac/MacTx",
                    MakeBoundCallback(&DeviceTxTrace, m_state.get()));
    Config::Connect("/NodeList/*/DeviceList/*/$ns3::WifiNetDevice/Mac/MacRx",
                    MakeBoundCallback(&DeviceRxTrace, m_state.get()));
}

} // namespace ns3
