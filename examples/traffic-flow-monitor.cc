#include "traffic-flow-monitor.h"

#include "scenario-log.h"
#include "traffic-coordinator.h"
#include "wifi-statistics.h"

#include "ns3/config.h"
#include "ns3/ipv4-header.h"
#include "ns3/llc-snap-header.h"
#include "ns3/packet.h"
#include "ns3/simulator.h"
#include "ns3/tcp-header.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <numeric>
#include <sstream>
#include <tuple>
#include <utility>
#include <vector>

namespace ns3
{

namespace
{

LogComponent& g_log = llm_example::GetScenarioLog();

struct FlowKey
{
    std::string sourceIp;
    uint16_t sourcePort;
    std::string destinationIp;
    uint16_t destinationPort;
    uint32_t payloadBytes;

    bool operator<(const FlowKey& other) const
    {
        return std::tie(sourceIp, sourcePort, destinationIp, destinationPort, payloadBytes) <
               std::tie(other.sourceIp,
                        other.sourcePort,
                        other.destinationIp,
                        other.destinationPort,
                        other.payloadBytes);
    }
};

struct ParsedPacket
{
    std::string sourceIp;
    uint16_t sourcePort;
    std::string destinationIp;
    uint16_t destinationPort;
    uint32_t payloadBytes;
};

std::string
ToString(Ipv4Address address)
{
    std::ostringstream stream;
    address.Print(stream);
    return stream.str();
}

bool
ParseTcpPayload(Ptr<const Packet> packet, ParsedPacket& parsed)
{
    Ptr<Packet> packetCopy = packet->Copy();
    LlcSnapHeader llc;
    packetCopy->RemoveHeader(llc);

    const uint32_t packetSize = packet->GetSize();
    if (packetSize <= 60)
    {
        return false;
    }

    Ipv4Header ipHeader;
    if (!packetCopy->PeekHeader(ipHeader))
    {
        return false;
    }

    parsed.sourceIp = ToString(ipHeader.GetSource());
    parsed.destinationIp = ToString(ipHeader.GetDestination());
    packetCopy->RemoveHeader(ipHeader);

    TcpHeader tcpHeader;
    if (!packetCopy->PeekHeader(tcpHeader))
    {
        return false;
    }

    parsed.sourcePort = tcpHeader.GetSourcePort();
    parsed.destinationPort = tcpHeader.GetDestinationPort();
    parsed.payloadBytes = packetSize - 60;
    return true;
}

void
DeviceTxTrace(TrafficFlowMonitor* monitor, std::string context, Ptr<const Packet> packet)
{
    monitor->RecordDeviceTx(std::move(context), packet);
}

void
DeviceRxTrace(TrafficFlowMonitor* monitor, std::string context, Ptr<const Packet> packet)
{
    monitor->RecordDeviceRx(std::move(context), packet);
}

} // namespace

struct TrafficFlowMonitorState
{
    std::map<std::string, std::vector<uint64_t>> transmittedBytesBySource;
    std::map<FlowKey, std::vector<uint64_t>> transmitTimestampsByFlow;
    std::map<FlowKey, std::vector<uint64_t>> receiveTimestampsByFlow;
};

TrafficFlowMonitor::TrafficFlowMonitor(const TrafficCoordinator& coordinator,
                                       WifiStatistics& wifiStatistics)
    : m_coordinator(coordinator),
      m_wifiStatistics(wifiStatistics),
      m_state(std::make_unique<TrafficFlowMonitorState>())
{
}

TrafficFlowMonitor::~TrafficFlowMonitor() = default;

void
TrafficFlowMonitor::ConnectDeviceTraces()
{
    Config::Connect("/NodeList/*/DeviceList/*/$ns3::WifiNetDevice/Mac/MacTx",
                    MakeBoundCallback(&DeviceTxTrace, this));
    Config::Connect("/NodeList/*/DeviceList/*/$ns3::WifiNetDevice/Mac/MacRx",
                    MakeBoundCallback(&DeviceRxTrace, this));
}

void
TrafficFlowMonitor::RecordDeviceTx(std::string, Ptr<const Packet> packet)
{
    const int64_t nowUs = Simulator::Now().GetMicroSeconds();
    const int64_t experimentStartUs = m_coordinator.GetExperimentStartUs();
    if (experimentStartUs < 0 || nowUs < experimentStartUs)
    {
        return;
    }

    ParsedPacket parsed;
    if (!ParseTcpPayload(packet, parsed))
    {
        return;
    }

    NS_LOG_INFO("TX [" << nowUs << " us] "
                       << "PayloadOnly: " << parsed.payloadBytes << " | "
                       << "tx: " << parsed.sourceIp << ":" << parsed.sourcePort << " -> "
                       << "rx: " << parsed.destinationIp << ":" << parsed.destinationPort);

    m_wifiStatistics.RecordMacPayload(nowUs,
                                      parsed.sourceIp,
                                      parsed.destinationIp,
                                      parsed.payloadBytes);

    FlowKey key{parsed.sourceIp,
                parsed.sourcePort,
                parsed.destinationIp,
                parsed.destinationPort,
                parsed.payloadBytes};
    m_state->transmitTimestampsByFlow[key].push_back(nowUs);
    m_state->transmittedBytesBySource[parsed.sourceIp].push_back(parsed.payloadBytes);
}

void
TrafficFlowMonitor::RecordDeviceRx(std::string, Ptr<const Packet> packet)
{
    const int64_t nowUs = Simulator::Now().GetMicroSeconds();
    const int64_t experimentStartUs = m_coordinator.GetExperimentStartUs();
    if (experimentStartUs < 0 || nowUs < experimentStartUs)
    {
        return;
    }

    ParsedPacket parsed;
    if (!ParseTcpPayload(packet, parsed))
    {
        return;
    }

    NS_LOG_INFO("RX [" << nowUs << " us] "
                       << "Payload: " << parsed.payloadBytes << " | "
                       << "tx: " << parsed.sourceIp << ":" << parsed.sourcePort << " -> "
                       << "rx: " << parsed.destinationIp << ":" << parsed.destinationPort);

    FlowKey key{parsed.sourceIp,
                parsed.sourcePort,
                parsed.destinationIp,
                parsed.destinationPort,
                parsed.payloadBytes};
    m_state->receiveTimestampsByFlow[key].push_back(nowUs);
}

void
TrafficFlowMonitor::PrintTransmissionTimePerSender() const
{
    NS_LOG_INFO("========== MAC Layer Transmission time per sender ==========");

    std::map<std::string, uint64_t> totalDurationUsBySender;
    std::map<std::string, uint64_t> totalBytesBySender;

    for (const auto& [flow, receiveTimestamps] : m_state->receiveTimestampsByFlow)
    {
        auto transmitIt = m_state->transmitTimestampsByFlow.find(flow);
        if (transmitIt == m_state->transmitTimestampsByFlow.end() || transmitIt->second.empty())
        {
            continue;
        }

        const auto& transmitTimestamps = transmitIt->second;
        const std::size_t matchedCount =
            std::min(transmitTimestamps.size(), receiveTimestamps.size());

        for (std::size_t index = 0; index < matchedCount; ++index)
        {
            const int64_t durationUs = static_cast<int64_t>(receiveTimestamps[index]) -
                                       static_cast<int64_t>(transmitTimestamps[index]);
            if (durationUs > 0)
            {
                totalDurationUsBySender[flow.sourceIp] += static_cast<uint64_t>(durationUs);
            }
        }
    }

    for (const auto& [sourceIp, byteSamples] : m_state->transmittedBytesBySource)
    {
        const int totalBytes = std::accumulate(byteSamples.begin(), byteSamples.end(), 0);
        totalBytesBySender[sourceIp] = totalBytes;
    }

    for (const auto& [sender, totalDurationUs] : totalDurationUsBySender)
    {
        const double totalMs = static_cast<double>(totalDurationUs) / 1000.0;
        const double totalSec = static_cast<double>(totalDurationUs) / 1e6;
        const double totalBytesMb =
            static_cast<double>(totalBytesBySender[sender]) / (1024.0 * 1024.0);
        NS_LOG_INFO("Sender " << sender << ": txTime=" << totalMs << " ms (" << totalSec << " s), "
                              << "PayloadOnly=" << totalBytesBySender[sender] << " ("
                              << totalBytesMb << " MB)"
                              << "effRate=" << totalBytesMb * 8 / totalSec << " mbps");
    }

    NS_LOG_INFO("============================================================");
}

} // namespace ns3
