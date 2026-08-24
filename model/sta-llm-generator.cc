// model/sta-llm-generator.cc
//
// Traffic Generator Application - TCP sender for station
// Based on ns3::OnOffApplication pattern
//

#include "sta-llm-generator.h"
#include "agent-distribution.h"

#include "ns3/inet-socket-address.h"
#include "ns3/inet6-socket-address.h"
#include "ns3/ipv4-address.h"
#include "ns3/log.h"
#include "ns3/nstime.h"
#include "ns3/packet.h"
#include "ns3/simulator.h"
#include "ns3/socket.h"
#include "ns3/trace-source-accessor.h"
#include "ns3/tcp-socket-factory.h"
#include "ns3/tcp-socket-base.h"
#include "ns3/uinteger.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("StaLlmGenerator");

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
                            "ns3::StaLlmGenerator::AgentSendCallback")
            .AddTraceSource("AgentSend",
                            "A complete agent transmission finished (start time, end time).",
                            MakeTraceSourceAccessor(&StaLlmGenerator::m_agentSendTrace),
                            "ns3::StaLlmGenerator::AgentSendCallback")
            .AddTraceSource("AppTxDrop",
                            "Application payload rejected by the TCP socket.",
                            MakeTraceSourceAccessor(&StaLlmGenerator::m_appTxDropTrace),
                            "ns3::StaLlmGenerator::AgentSendCallback");
    return tid;
}

StaLlmGenerator::StaLlmGenerator()
    : m_totalSent(0),
      m_currentCwnd(0)
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
  std::map<std::string, std::vector<std::tuple<int, double, double, int>>> agentsMap)
{
    NS_LOG_FUNCTION(this);
    m_agentsMap = agentsMap;
    m_allAgentKeys.clear();
    for (const auto &[agentKey, ops] : m_agentsMap)
    {
        if (std::find(m_allAgentKeys.begin(), m_allAgentKeys.end(), agentKey) == m_allAgentKeys.end()) {
            m_allAgentKeys.emplace_back(agentKey);
        }
    }

    // Merge all operations into a single vector and sort by startOffsetMs
    m_sortedOperations.clear();
    for (const auto &[agentKey, ops] : m_agentsMap)
    {
        for (const auto &[downlinkBytes, endMs, startOffsetMs, uplinkBytes] : ops)
        {
            m_sortedOperations.emplace_back(startOffsetMs,
                                            agentKey,
                                            downlinkBytes,
                                            endMs,
                                            uplinkBytes);
        }
    }

    // Sort by startOffsetMs (STA initiates uplink)
    std::sort(m_sortedOperations.begin(),
              m_sortedOperations.end(),
              [](const auto &a, const auto &b) { return std::get<0>(a) < std::get<0>(b); });

    NS_LOG_INFO("Merged " << m_sortedOperations.size() << " operations from "
                           << m_agentsMap.size() << " agents");
}

void
StaLlmGenerator::SetReadyCallback(Callback<void> callback)
{
    m_readyCallback = callback;
}

void
StaLlmGenerator::StartTraffic(uint64_t experimentStartMs)
{
    NS_ABORT_MSG_IF(!m_connected,
                    "StaLlmGenerator::StartTraffic called before TCP is connected");
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

    // Socket is already created by SourceApplication::StartApplication().
    // Subscribe to Cwnd trace via TcpSocketBase.
    auto tcpSocketBase = DynamicCast<TcpSocketBase>(m_socket);
    if (tcpSocketBase)
    {
        tcpSocketBase->TraceConnectWithoutContext(
          "CongestionWindow",
          MakeCallback(&StaLlmGenerator::OnCwndChange, this));
        tcpSocketBase->TraceConnectWithoutContext(
          "LastRTT",
          MakeCallback(&StaLlmGenerator::OnLastRttChange, this));

    }

    // Set receive callback to consume ACKs
    m_socket->SetRecvCallback(MakeCallback(&StaLlmGenerator::HandleRead, this));

    // Connection success only opens this generator's side of the readiness
    // barrier. Payload is scheduled later from the common global epoch.

    Address localAddress;
    m_socket->GetSockName(localAddress);
    InetSocketAddress inetLocal = InetSocketAddress::ConvertFrom(localAddress);
    auto localIp = inetLocal.GetIpv4();

    NS_LOG_INFO("StaLlmGenerator::DoStartApplication "
                << localIp);
}

void
StaLlmGenerator::DoStopApplication()
{
    NS_LOG_FUNCTION(this);

    Address localAddress;
    m_socket->GetSockName(localAddress);
    InetSocketAddress inetLocal = InetSocketAddress::ConvertFrom(localAddress);
    auto localIp = inetLocal.GetIpv4();

    if (!m_allAgentKeys.empty())
    {
        std::ostringstream notTxed;
        notTxed << "[";
        bool first = true;
        for (const auto &key : m_allAgentKeys)
        {
            if (!first)
            {
                notTxed << ", ";
            }
            notTxed << key;
            first = false;
        }
        notTxed << "]";
        NS_LOG_WARN("[StaLlmGenerator] Agents that didn't TX from station "
                    << localIp << ": " << notTxed.str());
    }

    NS_LOG_INFO("[StaLlmGenerator] stopping sta="
                << localIp
                << ", effective seconds="
                << m_perSecondStats.size());

    CancelEvents();
    PrintPerSecondMetrics();
}

void
StaLlmGenerator::CancelEvents()
{
    NS_LOG_FUNCTION(this);
    Simulator::Cancel(m_sendEvent);
}

void
StaLlmGenerator::DoConnectionSucceeded(Ptr<Socket> socket)
{
    NS_LOG_FUNCTION(this << socket);
    auto remoteIp = InetSocketAddress::ConvertFrom(m_peer).GetIpv4();

    Address localAddress;
    m_socket->GetSockName(localAddress);
    InetSocketAddress inetLocal = InetSocketAddress::ConvertFrom(localAddress);
    auto localIp = inetLocal.GetIpv4();

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

    for (uint32_t i = 0; i < m_sortedOperations.size(); ++i)
    {
        auto &[startOffsetMs, agentKey, downlinkBytes, endMs, uplinkBytes] =
          m_sortedOperations[i];

        const Time targetTime = Time::FromDouble(
            static_cast<double>(m_experimentStartMs) + startOffsetMs,
            Time::MS);

        NS_ABORT_MSG_IF(
            targetTime < Simulator::Now(),
            "StaLlmGenerator attempted to schedule payload before the common traffic epoch");

        const Time delay = targetTime - Simulator::Now();

        m_sendEvent = Simulator::Schedule(
          delay,
          &StaLlmGenerator::SendAgentData,
          this,
          agentKey,
          uplinkBytes,
          startOffsetMs);

        NS_LOG_INFO("[Agent " << agentKey
                               << "] Scheduled: trace time="
                               << std::fixed << std::setprecision(6)
                               << startOffsetMs
                               << "ms / simulation time="
                               << targetTime.As(Time::MS)
                               << " / delay="
                               << delay.As(Time::MS)
                               << " / uplinkBytes="
                               << uplinkBytes);

        auto it = std::find(m_allAgentKeys.begin(), m_allAgentKeys.end(), agentKey);
        if (it != m_allAgentKeys.end()) {
            m_allAgentKeys.erase(it);
        }
    }
    NS_LOG_INFO("NOW: " << Simulator::Now().As(Time::MS));
}

void
StaLlmGenerator::SendAgentData(std::string agentKey,
                                uint32_t bytes,
                                double scheduledMs)
{
    NS_LOG_FUNCTION(this << agentKey << bytes << scheduledMs);

    if (!m_socket || !m_connected)
    {
        NS_LOG_WARN("[Agent " << agentKey << "] Socket not connected");
        return;
    }

    Time startTime = Simulator::Now();
    Ptr<Packet> packet = Create<Packet>(bytes);

    Address localAddress;
    m_socket->GetSockName(localAddress);
    InetSocketAddress inetLocal = InetSocketAddress::ConvertFrom(localAddress);
    auto localIp = inetLocal.GetIpv4();
    const InetSocketAddress inetRemote = InetSocketAddress::ConvertFrom(m_peer);

    AppTxTag appTxTag(packet->GetUid(),
                      startTime.GetMicroSeconds(),
                      localIp,
                      inetRemote.GetIpv4(),
                      inetLocal.GetPort(),
                      inetRemote.GetPort(),
                      bytes,
                      agentKey);
    packet->AddByteTag(appTxTag);

    int sent = m_socket->Send(packet);
    if (sent < 0)
    {
        m_appTxDropTrace(agentKey, bytes, startTime);
        NS_LOG_ERROR("[" << localIp << " / " << agentKey << "] Failed to send " << bytes
                               << " bytes, sent=" << sent);
        return;
    }

    if (static_cast<uint32_t>(sent) < bytes)
    {
        m_appTxDropTrace(agentKey,
                         bytes - static_cast<uint32_t>(sent),
                         startTime);
    }

    const double actualTraceMs =
        startTime.GetSeconds() * 1000.0 -
        static_cast<double>(m_experimentStartMs);

    NS_LOG_WARN("[APP TX] direction=UL"
                << " agent=\"" << agentKey << "\""
                << " local=" << localIp
                << " remote=" << InetSocketAddress::ConvertFrom(m_peer).GetIpv4()
                << " trace_ms=" << std::fixed << std::setprecision(6) << scheduledMs
                << " actual_trace_ms=" << actualTraceMs
                << " delta_ms=" << (actualTraceMs - scheduledMs)
                << " expected_bytes=" << bytes
                << " socket_accepted_bytes=" << sent);

    m_totalSent += sent;
    m_agentBytesSent[agentKey] += sent;

    // Collect metrics into a one-second bucket.
    const uint32_t second =
      static_cast<uint32_t>(std::floor(startTime.GetSeconds()));

    PerSecondStats& stats = m_perSecondStats[second];

    stats.totalBytes += static_cast<uint64_t>(sent);
    stats.agentBytes[agentKey] += static_cast<uint64_t>(sent);
    stats.lastCwnd = m_currentCwnd;

    Time endTime = Simulator::Now();

    // NS_LOG_WARN("[" << localIp << " / " << agentKey << " / " << startTime.As(Time::MS) <<
    //                     "] Sent " << sent << " bytes, scheduledMs=" << scheduledMs << "ms"
    //                 );

    // Trace
    m_txTraceCustom(agentKey, bytes, startTime);
    m_agentSendTrace(agentKey, bytes, startTime, endTime);
}

void
StaLlmGenerator::HandleRead(Ptr<Socket> socket)
{
    NS_LOG_FUNCTION(this << socket);
    // Consume incoming data (ACKs, etc.)
    Address from;
    while (auto packet = socket->RecvFrom(from))
    {
        NS_LOG_DEBUG("Received " << packet->GetSize() << " bytes from " << from);
    }
}

// TODO: 'm_perSecondStats[second].lastCwnd' is frequently being rewritten,
// it is not correct to take the last one
void
StaLlmGenerator::OnCwndChange(uint32_t, uint32_t newCwnd)
{
    NS_LOG_FUNCTION(this << newCwnd);

     m_currentCwnd = static_cast<double>(newCwnd);

    const uint32_t second =
      static_cast<uint32_t>(
        std::floor(Simulator::Now().GetSeconds()));

    m_perSecondStats[second].lastCwnd = static_cast<double>(newCwnd);
}

// TODO: high RTT? 50ms for 5m distance - strange
void
StaLlmGenerator::OnLastRttChange(Time, Time lastRtt)
{
    NS_LOG_FUNCTION(this << lastRtt);

    if (lastRtt.GetMicroSeconds() == 0)
    {
        return;
    }

    // m_lastRttTrace(lastRtt.GetMicroSeconds());

    const uint32_t second =
      static_cast<uint32_t>(
        std::floor(Simulator::Now().GetSeconds()));

    auto &stats = m_perSecondStats[second];
    stats.rttSamples++;
    stats.rttSumUs += lastRtt.GetMicroSeconds();
}

void
StaLlmGenerator::PrintPerSecondMetrics()
{
    NS_LOG_FUNCTION(this);

    NS_LOG_WARN(
      "========== StaLlmGenerator per-second statistics ==========");

    if (m_perSecondStats.empty())
    {
        NS_LOG_WARN("[Final per-second] No transmitted data");
        NS_LOG_WARN(
          "==========================================================");
        return;
    }

    m_cwndSamples.clear();

    for (const auto& [second, stats] : m_perSecondStats)
    {
        if (stats.totalBytes == 0)
        {
            continue;
        }
        // Each bucket represents exactly one second.
        const double throughputBps =
          static_cast<double>(stats.totalBytes) * 8.0;

        const double throughputMbps =
          throughputBps / 1e6;

        // const double avgRttUs =
        //   stats.rttSamples > 0
        //     ? stats.rttSumUs / static_cast<double>(stats.rttSamples)
        //     : 0.0;

        NS_LOG_WARN(
          "[Final per-second] interval=["
          << static_cast<int64_t>(second) -
                 static_cast<int64_t>(m_experimentStartMs / 1000)
          << ","
          << static_cast<int64_t>(second + 1) -
                 static_cast<int64_t>(m_experimentStartMs / 1000)
          << ")s"
          << " totalBytes=" << stats.totalBytes
          << " throughput=" << throughputMbps << " Mbps"
          << " cwnd=" << stats.lastCwnd << " bytes");

        for (const auto& [agentKey, agentBytes] :
             stats.agentBytes)
        {
            const double agentThroughputMbps =
              static_cast<double>(agentBytes) * 8.0 / 1e6;

            const double bandwidthSharePercent =
              stats.totalBytes > 0
                ? static_cast<double>(agentBytes) /
                    static_cast<double>(stats.totalBytes) *
                    100.0
                : 0.0;

            NS_LOG_WARN(
              "[Final per-second]   Agent "
              << agentKey
              << ": bytes=" << agentBytes
              << " throughput=" << agentThroughputMbps
              << " Mbps"
              << " share=" << bandwidthSharePercent << "%");
        }

        m_cwndSamples.push_back(stats.lastCwnd);
    }

    // Overall average stats
    uint64_t totalBytesAllSeconds = 0;
    uint64_t seconds = 0;
    for (const auto& [second, stats] : m_perSecondStats)
    {
        if (stats.totalBytes == 0)
        {
            continue;
        }
        ++seconds;
        totalBytesAllSeconds += stats.totalBytes;
    }
    const double totalDurationSeconds = static_cast<double>(seconds);
    const double avgThroughputMbps = totalDurationSeconds > 0
      ? (static_cast<double>(totalBytesAllSeconds) * 8.0 / 1e6) / totalDurationSeconds
      : 0.0;

    double totalRttSumUs = 0.0;
    uint64_t totalRttSamples = 0;
    for (const auto& [second, stats] : m_perSecondStats)
    {
        totalRttSumUs += stats.rttSumUs;
        totalRttSamples += stats.rttSamples;
    }
    // double avgRttAll = totalRttSamples > 0
    //   ? totalRttSumUs / static_cast<double>(totalRttSamples)
    //   : 0.0;

    NS_LOG_WARN(
      "[Final overall] duration=" << totalDurationSeconds
      << "s totalBytes=" << totalBytesAllSeconds
      << " avgThroughput=" << avgThroughputMbps << " Mbps");

    NS_LOG_WARN(
      "==========================================================");
}

} // namespace ns3
