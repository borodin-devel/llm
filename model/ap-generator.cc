#include "ap-generator.h"

#include "app-tx-tag.h"
#include "llm-log.h"

#include "ns3/inet-socket-address.h"
#include "ns3/ipv4-address.h"
#include "ns3/log.h"
#include "ns3/nstime.h"
#include "ns3/packet.h"
#include "ns3/simulator.h"
#include "ns3/socket.h"
#include "ns3/tcp-socket-factory.h"
#include "ns3/uinteger.h"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <utility>

namespace ns3
{

static LogComponent& g_log = llm_detail::GetApGeneratorLog();

NS_OBJECT_ENSURE_REGISTERED(APGenerator);

TypeId
APGenerator::GetTypeId()
{
    static TypeId tid = TypeId("ns3::APGenerator")
                            .SetParent<Application>()
                            .SetGroupName("Applications")
                            .AddConstructor<APGenerator>()
                            .AddTraceSource("Tx",
                                            "Downlink packet sent to a station.",
                                            MakeTraceSourceAccessor(&APGenerator::m_txTrace),
                                            "ns3::APGenerator::AcceptedSendCallback")
                            .AddTraceSource("AgentSend",
                                            "A complete downlink transmission to a station.",
                                            MakeTraceSourceAccessor(&APGenerator::m_agentSendTrace),
                                            "ns3::APGenerator::AgentSendCallback")
                            .AddTraceSource("AppTxDrop",
                                            "Application payload rejected by the TCP socket.",
                                            MakeTraceSourceAccessor(&APGenerator::m_appTxDropTrace),
                                            "ns3::APGenerator::DropCallback");
    return tid;
}

APGenerator::APGenerator()
{
    NS_LOG_FUNCTION(this);
}

APGenerator::~APGenerator()
{
    NS_LOG_FUNCTION(this);
}

void
APGenerator::SetAgentStationMap(std::map<std::string, Address> stationAddressByAgent)
{
    NS_LOG_FUNCTION(this);
    m_stationAddressByAgent = std::move(stationAddressByAgent);
}

void
APGenerator::SetAgentMap(
    std::map<std::string, std::vector<std::tuple<int, double, double, int>>> operationsByAgent)
{
    NS_LOG_FUNCTION(this);
    m_operationsByAgent = std::move(operationsByAgent);
    m_unscheduledAgentKeys.clear();
    m_unscheduledAgentKeys.reserve(m_operationsByAgent.size());
    for (const auto& [agentKey, operations] : m_operationsByAgent)
    {
        (void)operations;
        m_unscheduledAgentKeys.push_back(agentKey);
    }
    BuildSchedules();
}

void
APGenerator::SetReadyCallback(Callback<void> callback)
{
    m_readyCallback = callback;
}

void
APGenerator::StartTraffic(uint64_t experimentStartMs)
{
    NS_ABORT_MSG_IF(!m_readyReported,
                    "APGenerator::StartTraffic called before all TCP connections are ready");
    NS_ABORT_MSG_IF(m_trafficStarted, "APGenerator::StartTraffic called more than once");

    m_trafficStarted = true;
    m_experimentStartMs = experimentStartMs;

    for (const auto& [station, operations] : m_downlinkSchedulesByStation)
    {
        (void)operations;
        ScheduleStationTransmissions(station);
    }
}

void
APGenerator::DoDispose()
{
    NS_LOG_FUNCTION(this);

    Ipv4Address localIp = Ipv4Address::GetAny();

    if (!m_socketByStation.empty() && m_socketByStation.begin()->second)
    {
        Address localAddress;
        m_socketByStation.begin()->second->GetSockName(localAddress);
        InetSocketAddress inetLocal = InetSocketAddress::ConvertFrom(localAddress);
        localIp = inetLocal.GetIpv4();
    }

    ReportUnscheduledAgents(localIp);

    for (auto& [stationAddress, socket] : m_socketByStation)
    {
        (void)stationAddress;
        socket = nullptr;
    }
    m_socketByStation.clear();
    m_socketToStation.clear();
    Application::DoDispose();
}

void
APGenerator::ReportUnscheduledAgents(Ipv4Address localAddress) const
{
    if (m_unscheduledAgentKeys.empty())
    {
        return;
    }

    std::ostringstream agentList;
    agentList << "[";
    for (std::size_t index = 0; index < m_unscheduledAgentKeys.size(); ++index)
    {
        if (index > 0)
        {
            agentList << ", ";
        }
        agentList << m_unscheduledAgentKeys[index];
    }
    agentList << "]";
    NS_LOG_WARN("[APGenerator] Agents that didn't TX from AP " << localAddress << ": "
                                                               << agentList.str());
}

void
APGenerator::StartApplication()
{
    NS_LOG_FUNCTION(this);

    ConnectToStations();

    // An AP with no mapped station has no TCP connection to wait for.
    ReportReadyIfComplete();
}

void
APGenerator::StopApplication()
{
    NS_LOG_FUNCTION(this);

    for (auto& [stationAddress, event] : m_sendEventByStation)
    {
        (void)stationAddress;
        Simulator::Cancel(event);
    }

    for (auto& [stationAddress, socket] : m_socketByStation)
    {
        (void)stationAddress;
        socket->Close();
    }
}

void
APGenerator::BuildSchedules()
{
    NS_LOG_FUNCTION(this);

    for (const auto& [agentKey, operations] : m_operationsByAgent)
    {
        const auto station = m_stationAddressByAgent.find(agentKey);
        if (station == m_stationAddressByAgent.end())
        {
            NS_LOG_WARN("Agent " << agentKey << " not in station map, skipping");
            continue;
        }
        else
        {
            NS_LOG_INFO("FOUND " << agentKey);
        }

        (void)operations;
        NS_LOG_INFO("AggregateAndSortOperations "
                    << InetSocketAddress::ConvertFrom(station->second).GetIpv4());
    }

    m_downlinkSchedulesByStation =
        BuildDownlinkSchedules(m_operationsByAgent, m_stationAddressByAgent);

    NS_LOG_WARN("Aggregated operations for " << m_downlinkSchedulesByStation.size()
                                             << " stations from " << m_operationsByAgent.size()
                                             << " agents");
}

void
APGenerator::ConnectToStations()
{
    NS_LOG_FUNCTION(this);

    for (const auto& [stationAddress, schedule] : m_downlinkSchedulesByStation)
    {
        (void)schedule;
        NS_LOG_INFO("ConnectToStations "
                    << InetSocketAddress::ConvertFrom(stationAddress).GetIpv4() << " "
                    << InetSocketAddress::ConvertFrom(stationAddress).GetPort());
        Ptr<Socket> socket = Socket::CreateSocket(GetNode(), TcpSocketFactory::GetTypeId());
        ConfigureSocket(stationAddress, socket);

        NS_LOG_WARN("Created socket for station " << stationAddress);
    }
}

void
APGenerator::ConfigureSocket(const Address& stationAddress, Ptr<Socket> socket)
{
    m_socketToStation[&*socket] = stationAddress;
    m_socketByStation[stationAddress] = socket;
    m_isConnectedByStation[stationAddress] = false;
    m_sendEventByStation[stationAddress] = EventId();

    socket->SetConnectCallback(MakeCallback(&APGenerator::OnConnectionSucceeded, this),
                               MakeCallback(&APGenerator::OnConnectionFailed, this));
    socket->SetRecvCallback(MakeCallback(&APGenerator::HandleRead, this));

    socket->Connect(stationAddress);
}

void
APGenerator::ScheduleStationTransmissions(const Address& stationAddress)
{
    NS_LOG_FUNCTION(this);

    const auto& schedule = m_downlinkSchedulesByStation.at(stationAddress);

    for (const auto& payload : schedule)
    {
        const Time targetTime =
            GetScheduledSimulationTime(m_experimentStartMs, payload.traceTimeMs);

        NS_ABORT_MSG_IF(
            targetTime < Simulator::Now(),
            "APGenerator attempted to schedule payload before the common traffic epoch");

        const Time delay = targetTime - Simulator::Now();

        m_sendEventByStation[stationAddress] = Simulator::Schedule(delay,
                                                                   &APGenerator::SendDownlink,
                                                                   this,
                                                                   stationAddress,
                                                                   payload.agentKey,
                                                                   payload.payloadBytes,
                                                                   payload.traceTimeMs);

        NS_LOG_INFO("[Agent " << payload.agentKey << "] Scheduled: trace time=" << std::fixed
                              << std::setprecision(6) << payload.traceTimeMs
                              << "ms / simulation time=" << targetTime.As(Time::MS) << " / delay="
                              << delay.As(Time::MS) << " / downlinkBytes=" << payload.payloadBytes);

        auto unscheduledAgent = std::find(m_unscheduledAgentKeys.begin(),
                                          m_unscheduledAgentKeys.end(),
                                          payload.agentKey);
        if (unscheduledAgent != m_unscheduledAgentKeys.end())
        {
            m_unscheduledAgentKeys.erase(unscheduledAgent);
        }
    }

    NS_LOG_INFO("NOW: " << Simulator::Now().As(Time::MS));
}

void
APGenerator::SendDownlink(const Address& stationAddress,
                          const std::string& agentKey,
                          uint32_t payloadBytes,
                          double traceTimeMs)
{
    NS_LOG_FUNCTION(this << stationAddress << agentKey << payloadBytes << traceTimeMs);

    auto socketIt = m_socketByStation.find(stationAddress);
    if (socketIt == m_socketByStation.end() || !socketIt->second)
    {
        NS_LOG_WARN("No socket for station " << stationAddress);
        return;
    }

    Ptr<Socket> socket = socketIt->second;
    if (!m_isConnectedByStation[stationAddress])
    {
        NS_LOG_WARN("Station " << stationAddress << " not connected yet, skipping send");
        return;
    }

    const Time transmitTime = Simulator::Now();
    Ptr<Packet> packet = Create<Packet>(payloadBytes);

    Address localAddress;
    socket->GetSockName(localAddress);
    InetSocketAddress inetLocal = InetSocketAddress::ConvertFrom(localAddress);
    auto localIp = inetLocal.GetIpv4();

    const InetSocketAddress inetRemote = InetSocketAddress::ConvertFrom(stationAddress);
    auto remoteIp = inetRemote.GetIpv4();

    AddAppTxTag(packet, transmitTime, inetLocal, inetRemote, agentKey);

    const int acceptedBytes = socket->Send(packet);
    if (!EmitSendResult(stationAddress, agentKey, payloadBytes, acceptedBytes, transmitTime))
    {
        NS_LOG_ERROR("[AP] " << localIp << " Failed to send " << payloadBytes << " bytes to "
                             << remoteIp << " for agent " << agentKey);
        return;
    }

    const double actualTraceMs =
        transmitTime.GetSeconds() * 1000.0 - static_cast<double>(m_experimentStartMs);

    NS_LOG_WARN("[APP TX] direction=DL"
                << " agent=\"" << agentKey << "\""
                << " local=" << localIp << " remote=" << remoteIp << " trace_ms=" << std::fixed
                << std::setprecision(6) << traceTimeMs << " actual_trace_ms=" << actualTraceMs
                << " delta_ms=" << (actualTraceMs - traceTimeMs) << " expected_bytes="
                << payloadBytes << " socket_accepted_bytes=" << acceptedBytes);

    const Time endTime = Simulator::Now();

    m_agentSendTrace(stationAddress, agentKey, payloadBytes, transmitTime, endTime);
}

bool
APGenerator::EmitSendResult(const Address& stationAddress,
                            const std::string& agentKey,
                            uint32_t requestedBytes,
                            int acceptedBytes,
                            Time transmitTime)
{
    if (acceptedBytes < 0)
    {
        m_appTxDropTrace(stationAddress, agentKey, requestedBytes, transmitTime);
        return false;
    }

    const auto acceptedPayloadBytes = static_cast<uint32_t>(acceptedBytes);
    if (acceptedPayloadBytes < requestedBytes)
    {
        m_appTxDropTrace(stationAddress,
                         agentKey,
                         requestedBytes - acceptedPayloadBytes,
                         transmitTime);
    }
    m_txTrace(stationAddress, agentKey, acceptedPayloadBytes, transmitTime);
    return true;
}

void
APGenerator::HandleRead(Ptr<Socket> socket)
{
    NS_LOG_FUNCTION(this << socket);
    Address from;
    while (auto packet = socket->RecvFrom(from))
    {
        Address localAddress;
        socket->GetSockName(localAddress);
        InetSocketAddress inetLocal = InetSocketAddress::ConvertFrom(localAddress);
        auto localIp = inetLocal.GetIpv4();

        NS_LOG_DEBUG("[AP] " << localIp << " Received " << packet->GetSize() << " bytes");
    }
}

void
APGenerator::OnConnectionSucceeded(Ptr<Socket> socket)
{
    NS_LOG_FUNCTION(this << socket);

    const auto stationEntry = m_socketToStation.find(&*socket);
    if (stationEntry == m_socketToStation.end())
    {
        NS_LOG_ERROR("[AP] Unknown socket in connection succeeded");
        return;
    }
    const Address& stationAddress = stationEntry->second;
    const Ipv4Address remoteIp = InetSocketAddress::ConvertFrom(stationAddress).GetIpv4();

    m_isConnectedByStation[stationAddress] = true;
    NS_LOG_INFO("[AP] Connected to station " << remoteIp);

    ReportReadyIfComplete();
}

void
APGenerator::ReportReadyIfComplete()
{
    if (m_readyReported)
    {
        return;
    }

    for (const auto& [station, connected] : m_isConnectedByStation)
    {
        (void)station;
        if (!connected)
        {
            return;
        }
    }

    m_readyReported = true;

    if (!m_readyCallback.IsNull())
    {
        m_readyCallback();
    }
}

void
APGenerator::OnConnectionFailed(Ptr<Socket> socket)
{
    NS_LOG_FUNCTION(this << socket);

    const auto stationEntry = m_socketToStation.find(&*socket);
    if (stationEntry == m_socketToStation.end())
    {
        NS_LOG_ERROR("[AP] Unknown socket in connection failed");
        return;
    }
    const Address& stationAddress = stationEntry->second;

    m_isConnectedByStation[stationAddress] = false;
    NS_FATAL_ERROR("[AP] Failed to connect to station " << stationAddress);
}

} // namespace ns3
