#include "sta-llm-generator.h"

#include "app-tx-tag.h"
#include "llm-log.h"

#include "ns3/inet-socket-address.h"
#include "ns3/inet6-socket-address.h"
#include "ns3/ipv4-address.h"
#include "ns3/log.h"
#include "ns3/nstime.h"
#include "ns3/packet.h"
#include "ns3/simulator.h"
#include "ns3/socket.h"
#include "ns3/tcp-socket-factory.h"
#include "ns3/trace-source-accessor.h"
#include "ns3/uinteger.h"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <utility>

namespace ns3
{

static LogComponent& g_log = llm_detail::GetStaLlmGeneratorLog();

NS_OBJECT_ENSURE_REGISTERED(StaLlmGenerator);

TypeId
StaLlmGenerator::GetTypeId()
{
    static TypeId tid =
        TypeId("ns3::StaLlmGenerator")
            .SetParent<SourceApplication>()
            .SetGroupName("Applications")
            .AddConstructor<StaLlmGenerator>()
            .AddAttribute("Protocol",
                          "The protocol type, defaults to UdpSocketFactory.",
                          TypeIdValue(TcpSocketFactory::GetTypeId()),
                          MakeTypeIdAccessor(&StaLlmGenerator::m_protocolTid),
                          MakeTypeIdChecker())
            .AddTraceSource("TxCustom",
                            "A packet is created and scheduled for transmission.",
                            MakeTraceSourceAccessor(&StaLlmGenerator::m_txTraceCustom),
                            "ns3::StaLlmGenerator::AcceptedSendCallback")
            .AddTraceSource("AgentSend",
                            "A complete agent transmission finished (start time, end time).",
                            MakeTraceSourceAccessor(&StaLlmGenerator::m_agentSendTrace),
                            "ns3::StaLlmGenerator::AgentSendCallback")
            .AddTraceSource("AppTxDrop",
                            "Application payload rejected by the TCP socket.",
                            MakeTraceSourceAccessor(&StaLlmGenerator::m_appTxDropTrace),
                            "ns3::StaLlmGenerator::DropCallback");
    return tid;
}

StaLlmGenerator::StaLlmGenerator()
{
    NS_LOG_FUNCTION(this);
}

StaLlmGenerator::~StaLlmGenerator()
{
    NS_LOG_FUNCTION(this);
}

void
StaLlmGenerator::SetAgentIds(std::vector<int64_t> agentIds)
{
    NS_LOG_FUNCTION(this);
    (void)agentIds;
}

void
StaLlmGenerator::SetAgentMap(
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

    m_uplinkSchedule = BuildUplinkSchedule(m_operationsByAgent);

    NS_LOG_INFO("Merged " << m_uplinkSchedule.size() << " operations from "
                          << m_operationsByAgent.size() << " agents");
}

void
StaLlmGenerator::SetReadyCallback(Callback<void> callback)
{
    m_readyCallback = callback;
}

void
StaLlmGenerator::StartTraffic(uint64_t experimentStartMs)
{
    NS_ABORT_MSG_IF(!m_connected, "StaLlmGenerator::StartTraffic called before TCP is connected");
    NS_ABORT_MSG_IF(m_transmissionsScheduled,
                    "StaLlmGenerator::StartTraffic called more than once");

    m_experimentStartMs = experimentStartMs;
    m_transmissionsScheduled = true;
    ScheduleAllTransmissions();
}

void
StaLlmGenerator::DoDispose()
{
    NS_LOG_FUNCTION(this);
    SourceApplication::DoDispose();
}

void
StaLlmGenerator::DoStartApplication()
{
    NS_LOG_FUNCTION(this);

    m_socket->SetRecvCallback(MakeCallback(&StaLlmGenerator::HandleRead, this));

    // Connection success only opens this generator's side of the readiness
    // barrier. Payload is scheduled later from the common global epoch.

    Address localAddress;
    m_socket->GetSockName(localAddress);
    const InetSocketAddress localSocketAddress = InetSocketAddress::ConvertFrom(localAddress);
    const Ipv4Address localIp = localSocketAddress.GetIpv4();

    NS_LOG_INFO("StaLlmGenerator::DoStartApplication " << localIp);
}

void
StaLlmGenerator::DoStopApplication()
{
    NS_LOG_FUNCTION(this);

    Address localAddress;
    m_socket->GetSockName(localAddress);
    const InetSocketAddress localSocketAddress = InetSocketAddress::ConvertFrom(localAddress);
    const Ipv4Address localIp = localSocketAddress.GetIpv4();

    ReportUnscheduledAgents(localIp);

    CancelEvents();
}

void
StaLlmGenerator::CancelEvents()
{
    NS_LOG_FUNCTION(this);
    Simulator::Cancel(m_sendEvent);
}

void
StaLlmGenerator::ReportUnscheduledAgents(Ipv4Address localAddress) const
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
    NS_LOG_WARN("[StaLlmGenerator] Agents that didn't TX from station " << localAddress << ": "
                                                                        << agentList.str());
}

void
StaLlmGenerator::DoConnectionSucceeded(Ptr<Socket> socket)
{
    NS_LOG_FUNCTION(this << socket);
    const Ipv4Address remoteIp = InetSocketAddress::ConvertFrom(m_peer).GetIpv4();

    Address localAddress;
    m_socket->GetSockName(localAddress);
    const InetSocketAddress localSocketAddress = InetSocketAddress::ConvertFrom(localAddress);
    const Ipv4Address localIp = localSocketAddress.GetIpv4();

    NS_LOG_INFO("TCP connection from " << localIp << " established to " << remoteIp);

    if (!m_readyReported)
    {
        m_readyReported = true;
        if (!m_readyCallback.IsNull())
        {
            m_readyCallback();
        }
    }
}

void
StaLlmGenerator::DoConnectionFailed(Ptr<Socket> socket)
{
    NS_LOG_FUNCTION(this << socket);
    NS_FATAL_ERROR("Failed to connect to " << m_peer);
}

void
StaLlmGenerator::ScheduleAllTransmissions()
{
    NS_LOG_FUNCTION(this);

    for (const auto& payload : m_uplinkSchedule)
    {
        const Time targetTime =
            GetScheduledSimulationTime(m_experimentStartMs, payload.traceTimeMs);

        NS_ABORT_MSG_IF(
            targetTime < Simulator::Now(),
            "StaLlmGenerator attempted to schedule payload before the common traffic epoch");

        const Time delay = targetTime - Simulator::Now();

        m_sendEvent = Simulator::Schedule(delay,
                                          &StaLlmGenerator::SendAgentData,
                                          this,
                                          payload.agentKey,
                                          payload.payloadBytes,
                                          payload.traceTimeMs);

        NS_LOG_INFO("[Agent " << payload.agentKey << "] Scheduled: trace time=" << std::fixed
                              << std::setprecision(6) << payload.traceTimeMs
                              << "ms / simulation time=" << targetTime.As(Time::MS) << " / delay="
                              << delay.As(Time::MS) << " / uplinkBytes=" << payload.payloadBytes);

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
StaLlmGenerator::SendAgentData(std::string agentKey, uint32_t payloadBytes, double traceTimeMs)
{
    NS_LOG_FUNCTION(this << agentKey << payloadBytes << traceTimeMs);

    if (!m_socket || !m_connected)
    {
        NS_LOG_WARN("[Agent " << agentKey << "] Socket not connected");
        return;
    }

    const Time transmitTime = Simulator::Now();
    Ptr<Packet> packet = Create<Packet>(payloadBytes);

    Address localAddress;
    m_socket->GetSockName(localAddress);
    const InetSocketAddress localSocketAddress = InetSocketAddress::ConvertFrom(localAddress);
    const Ipv4Address localIp = localSocketAddress.GetIpv4();
    const InetSocketAddress remoteSocketAddress = InetSocketAddress::ConvertFrom(m_peer);

    AddAppTxTag(packet, transmitTime, localSocketAddress, remoteSocketAddress, agentKey);

    const int acceptedBytes = m_socket->Send(packet);
    if (!EmitSendResult(agentKey, payloadBytes, acceptedBytes, transmitTime))
    {
        NS_LOG_ERROR("[" << localIp << " / " << agentKey << "] Failed to send " << payloadBytes
                         << " bytes, sent=" << acceptedBytes);
        return;
    }

    const double actualTraceMs =
        transmitTime.GetSeconds() * 1000.0 - static_cast<double>(m_experimentStartMs);

    NS_LOG_WARN("[APP TX] direction=UL"
                << " agent=\"" << agentKey << "\""
                << " local=" << localIp << " remote=" << remoteSocketAddress.GetIpv4()
                << " trace_ms=" << std::fixed << std::setprecision(6) << traceTimeMs
                << " actual_trace_ms=" << actualTraceMs << " delta_ms="
                << (actualTraceMs - traceTimeMs) << " expected_bytes=" << payloadBytes
                << " socket_accepted_bytes=" << acceptedBytes);

    const Time endTime = Simulator::Now();

    m_agentSendTrace(agentKey, payloadBytes, transmitTime, endTime);
}

bool
StaLlmGenerator::EmitSendResult(const std::string& agentKey,
                                uint32_t requestedBytes,
                                int acceptedBytes,
                                Time transmitTime)
{
    if (acceptedBytes < 0)
    {
        m_appTxDropTrace(agentKey, requestedBytes, transmitTime);
        return false;
    }

    const auto acceptedPayloadBytes = static_cast<uint32_t>(acceptedBytes);
    if (acceptedPayloadBytes < requestedBytes)
    {
        m_appTxDropTrace(agentKey, requestedBytes - acceptedPayloadBytes, transmitTime);
    }
    m_txTraceCustom(agentKey, acceptedPayloadBytes, transmitTime);
    return true;
}

void
StaLlmGenerator::HandleRead(Ptr<Socket> socket)
{
    NS_LOG_FUNCTION(this << socket);
    Address from;
    while (auto packet = socket->RecvFrom(from))
    {
        NS_LOG_DEBUG("Received " << packet->GetSize() << " bytes from " << from);
    }
}

} // namespace ns3
