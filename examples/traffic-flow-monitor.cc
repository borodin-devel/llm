#include "traffic-flow-monitor.h"

#include "scenario-log.h"
#include "traffic-coordinator.h"
#include "traffic-flow-monitor-internal.h"
#include "wifi-statistics.h"

#include "ns3/config.h"
#include "ns3/ipv4-header.h"
#include "ns3/llc-snap-header.h"
#include "ns3/packet.h"
#include "ns3/simulator.h"
#include "ns3/tcp-header.h"

#include <sstream>
#include <utility>

namespace ns3
{

namespace
{

LogComponent& g_log = llm_example::GetScenarioLog();

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

    TrafficFlowKey key{parsed.sourceIp,
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

    TrafficFlowKey key{parsed.sourceIp,
                       parsed.sourcePort,
                       parsed.destinationIp,
                       parsed.destinationPort,
                       parsed.payloadBytes};
    m_state->receiveTimestampsByFlow[key].push_back(nowUs);
}

TransmissionSummary
TrafficFlowMonitor::BuildTransmissionSummary() const
{
    return ns3::BuildTransmissionSummary(*m_state);
}

void
TrafficFlowMonitor::PrintTransmissionTimePerSender() const
{
    const TransmissionSummary summary = BuildTransmissionSummary();

    NS_LOG_INFO("========== MAC Layer Transmission time per sender ==========");
    for (const auto& sender : summary.senders)
    {
        const double totalMs = static_cast<double>(sender.totalTransmissionDurationUs) / 1000.0;
        const double totalSec = static_cast<double>(sender.totalTransmissionDurationUs) / 1e6;
        const double totalBytesMb =
            static_cast<double>(sender.transmittedPayloadBytes) / (1024.0 * 1024.0);
        NS_LOG_INFO("Sender " << sender.senderIpv4 << ": matched=" << sender.matchedPacketCount
                              << ", txTime=" << totalMs << " ms (" << totalSec << " s), "
                              << "PayloadOnly=" << sender.transmittedPayloadBytes << " ("
                              << totalBytesMb << " MB), effRate="
                              << (sender.effectiveThroughputMbps
                                      ? std::to_string(*sender.effectiveThroughputMbps)
                                      : std::string{"null"})
                              << " Mbps");
    }

    NS_LOG_INFO("============================================================");
}

} // namespace ns3
