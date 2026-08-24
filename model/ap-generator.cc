// model/ap-generator.cc
//
// AP Generator Application - Downlink sender from Access Point
// Sends downlink data to stations based on agent-to-station mapping
//

#include "ap-generator.h"
#include "agent-distribution.h"

#include "ns3/inet-socket-address.h"
#include "ns3/ipv4-address.h"
#include "ns3/log.h"
#include "ns3/nstime.h"
#include "ns3/packet.h"
#include "ns3/simulator.h"
#include "ns3/socket.h"
#include "ns3/tcp-socket-factory.h"
#include "ns3/tcp-socket-base.h"
#include "ns3/uinteger.h"

#include <algorithm>
#include <cmath>
#include <iomanip>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("APGenerator");

NS_OBJECT_ENSURE_REGISTERED(APGenerator);

TypeId
APGenerator::GetTypeId()
{
    static TypeId tid =
        TypeId("ns3::APGenerator")
            .SetParent<Application>()
            .SetGroupName("Applications")
            .AddConstructor<APGenerator>()
            .AddTraceSource("Tx",
                            "Downlink packet sent to a station.",
                            MakeTraceSourceAccessor(&APGenerator::m_txTrace),
                            "ns3::APGenerator::AgentSendCallback")
            .AddTraceSource("AgentSend",
                            "A complete downlink transmission to a station.",
                            MakeTraceSourceAccessor(&APGenerator::m_agentSendTrace),
                            "ns3::APGenerator::AgentSendCallback")
            .AddTraceSource("AppTxDrop",
                            "Application payload rejected by the TCP socket.",
                            MakeTraceSourceAccessor(&APGenerator::m_appTxDropTrace),
                            "ns3::APGenerator::AgentSendCallback");
    return tid;
}

APGenerator::APGenerator()
    : m_totalSent(0)
{
    NS_LOG_FUNCTION(this);
}

APGenerator::~APGenerator()
{
    NS_LOG_FUNCTION(this);
}

void
APGenerator::SetAgentStationMap(std::map<std::string, Address> mapping)
{
    NS_LOG_FUNCTION(this);
    m_agentStationMap = mapping;
}

void
APGenerator::SetAgentMap(
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
    AggregateAndSortOperations();
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
    NS_ABORT_MSG_IF(m_trafficStarted,
                    "APGenerator::StartTraffic called more than once");

    m_trafficStarted = true;
    m_experimentStartMs = experimentStartMs;

    for (const auto& [station, operations] : m_stationOperations)
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

    if (!m_stationSockets.empty() && m_stationSockets.begin()->second)
    {
        Address localAddress;
        m_stationSockets.begin()->second->GetSockName(localAddress);
        InetSocketAddress inetLocal = InetSocketAddress::ConvertFrom(localAddress);
        localIp = inetLocal.GetIpv4();
    }

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
        NS_LOG_WARN("[APGenerator] Agents that didn't TX from AP "
                    << localIp << ": " << notTxed.str());
    }

    NS_LOG_INFO("[APGenerator] stopping ap="
                << localIp
                << ", effective seconds="
                << m_perSecondStats.size());

    PrintPerSecondMetrics();

    for (auto &[addr, socket] : m_stationSockets)
    {
        socket = nullptr;
    }
    m_stationSockets.clear();
    m_socketToStation.clear();
    Application::DoDispose();
}

void
APGenerator::StartApplication()
{
    NS_LOG_FUNCTION(this);

    // Create sockets and connect to each station.
    ConnectToStations();

    // An AP with no mapped station has no TCP connection to wait for.
    ReportReadyIfComplete();
}

void
APGenerator::StopApplication()
{
    NS_LOG_FUNCTION(this);

    // Cancel all pending events
    for (auto &[addr, event] : m_stationSendEvents)
    {
        Simulator::Cancel(event);
    }

    // Close all sockets
    for (auto &[addr, socket] : m_stationSockets)
    {
        socket->Close();
    }
}

void
APGenerator::AggregateAndSortOperations()
{
    NS_LOG_FUNCTION(this);

    m_stationOperations.clear();

    for (const auto &[agentKey, ops] : m_agentsMap)
    {
        auto it = m_agentStationMap.find(agentKey);
        if (it == m_agentStationMap.end())
        {
            NS_LOG_WARN("Agent " << agentKey << " not in station map, skipping");
            continue;
        } else {
            NS_LOG_INFO("FOUND " << agentKey);
        }

        const Address &station = it->second;
        NS_LOG_INFO("AggregateAndSortOperations " << InetSocketAddress::ConvertFrom(station).GetIpv4());

        for (const auto &[downlinkBytes, endMs, startMs, uplinkBytes] : ops)
        {
            m_stationOperations[station].push_back(StationOperation{
              agentKey,
              static_cast<uint32_t>(downlinkBytes),
              endMs,
              startMs});
        }
    }

    // Sort each station's operations by endMs (AP sends downlink after UL/wait)
    for (auto &[station, ops] : m_stationOperations)
    {
        std::sort(ops.begin(),
                  ops.end(),
                  [](const StationOperation &a, const StationOperation &b) {
                      return a.endMs < b.endMs;
                  });
    }

    NS_LOG_WARN("Aggregated operations for " << m_stationOperations.size()
                                             << " stations from "
                                             << m_agentsMap.size() << " agents");
}

void
APGenerator::ConnectToStations()
{
    NS_LOG_FUNCTION(this);

    for (const auto &[station, ops] : m_stationOperations)
    {
        NS_LOG_INFO("ConnectToStations " << InetSocketAddress::ConvertFrom(station).GetIpv4() <<
                " " << InetSocketAddress::ConvertFrom(station).GetPort());
        // Create TCP socket for this station
        Ptr<Socket> socket = Socket::CreateSocket(GetNode(), TcpSocketFactory::GetTypeId());

        // Store station address in reverse map (socket pointer -> station)
        m_socketToStation[&*socket] = station;
        m_stationSockets[station] = socket;
        m_stationConnected[station] = false;
        m_stationSendEvents[station] = EventId();

        // Set up callbacks
        socket->SetConnectCallback(
          MakeCallback(&APGenerator::OnConnectionSucceeded, this),
          MakeCallback(&APGenerator::OnConnectionFailed, this));

        socket->SetRecvCallback(MakeCallback(&APGenerator::HandleRead, this));

        // Subscribe to Cwnd trace via TcpSocketBase
        auto tcpBase = DynamicCast<TcpSocketBase>(socket);
        if (tcpBase)
        {
            tcpBase->TraceConnectWithoutContext(
              "CongestionWindow",
              MakeCallback(&APGenerator::OnCwndChange, this));
        }

        socket->Connect(station);

        NS_LOG_WARN("Created socket for station " << station);
    }
}

void
APGenerator::ScheduleStationTransmissions(const Address &station)
{
    NS_LOG_FUNCTION(this);

    const auto &ops = m_stationOperations.at(station);

    for (const auto &op : ops)
    {
        const Time targetTime = Time::FromDouble(
            static_cast<double>(m_experimentStartMs) + op.endMs,
            Time::MS);

        NS_ABORT_MSG_IF(
            targetTime < Simulator::Now(),
            "APGenerator attempted to schedule payload before the common traffic epoch");

        const Time delay = targetTime - Simulator::Now();

        m_stationSendEvents[station] = Simulator::Schedule(
          delay,
          &APGenerator::SendDownlink,
          this,
          station,
          op.agentKey,
          op.downlinkBytes,
          op.endMs);

        NS_LOG_INFO("[Agent " << op.agentKey
                               << "] Scheduled: trace time="
                               << std::fixed << std::setprecision(6)
                               << op.endMs
                               << "ms / simulation time="
                               << targetTime.As(Time::MS)
                               << " / delay="
                               << delay.As(Time::MS)
                               << " / downlinkBytes="
                               << op.downlinkBytes);

        auto it = std::find(m_allAgentKeys.begin(), m_allAgentKeys.end(), op.agentKey);
        if (it != m_allAgentKeys.end()) {
            m_allAgentKeys.erase(it);
        }
    }

    NS_LOG_INFO("NOW: " << Simulator::Now().As(Time::MS));
}

void
APGenerator::SendDownlink(const Address &station,
                          const std::string &agentKey,
                          uint32_t bytes,
                          double startMs)
{
    NS_LOG_FUNCTION(this << station << agentKey << bytes << startMs);

    auto socketIt = m_stationSockets.find(station);
    if (socketIt == m_stationSockets.end() || !socketIt->second)
    {
        NS_LOG_WARN("No socket for station " << station);
        return;
    }

    Ptr<Socket> socket = socketIt->second;
    if (!m_stationConnected[station])
    {
        NS_LOG_WARN("Station " << station << " not connected yet, skipping send");
        return;
    }

    Time startTime = Simulator::Now();
    Ptr<Packet> packet = Create<Packet>(bytes);

    // Per-station metrics
    auto &metrics = m_stationMetrics[station];

    Address localAddress;
    socket->GetSockName(localAddress);
    InetSocketAddress inetLocal = InetSocketAddress::ConvertFrom(localAddress);
    auto localIp = inetLocal.GetIpv4();

    const InetSocketAddress inetRemote = InetSocketAddress::ConvertFrom(station);
    auto remoteIp = inetRemote.GetIpv4();

    // Attach metadata to the application payload bytes. Byte tags survive TCP
    // segmentation/merging and can be recovered from the MPDU at PhyTxBegin.
    AppTxTag appTxTag(packet->GetUid(),
                      startTime.GetMicroSeconds(),
                      localIp,
                      remoteIp,
                      inetLocal.GetPort(),
                      inetRemote.GetPort(),
                      bytes,
                      agentKey);
    packet->AddByteTag(appTxTag);

    int sent = socket->Send(packet);
    if (sent < 0)
    {
        m_appTxDropTrace(station, agentKey, bytes, startTime);
        NS_LOG_ERROR("[AP] " << localIp << " Failed to send " << bytes << " bytes to " << remoteIp
                                            << " for agent " << agentKey);
        return;
    }

    if (static_cast<uint32_t>(sent) < bytes)
    {
        m_appTxDropTrace(station,
                         agentKey,
                         bytes - static_cast<uint32_t>(sent),
                         startTime);
    }

    const double actualTraceMs =
        startTime.GetSeconds() * 1000.0 -
        static_cast<double>(m_experimentStartMs);

    NS_LOG_WARN("[APP TX] direction=DL"
                << " agent=\"" << agentKey << "\""
                << " local=" << localIp
                << " remote=" << remoteIp
                << " trace_ms=" << std::fixed << std::setprecision(6) << startMs
                << " actual_trace_ms=" << actualTraceMs
                << " delta_ms=" << (actualTraceMs - startMs)
                << " expected_bytes=" << bytes
                << " socket_accepted_bytes=" << sent);

    m_totalSent += sent;
    m_stationBytesSent[station] += sent;
    metrics.bytesSent += sent;

    // Collect metrics into a one-second bucket.
    const uint32_t second =
      static_cast<uint32_t>(std::floor(startTime.GetSeconds()));

    PerSecondStats& stats = m_perSecondStats[second];

    stats.totalBytes += static_cast<uint64_t>(sent);
    stats.agentBytes[agentKey] += static_cast<uint64_t>(sent);
    stats.stationBytes[station] += static_cast<uint64_t>(sent);

    stats.lastCwnd = m_stationMetrics[station].currentCwnd;

    Time endTime = Simulator::Now();

    // NS_LOG_WARN("[AP] " << localIp << " Sent to [ " <<
    //                         remoteIp << " / " << agentKey << " / " << startTime.As(Time::MS) <<
    //                         " ]" << sent << " bytes, scheduledMs=" << startMs << "ms");

    // Trace
    m_txTrace(station, agentKey, bytes, startTime);
    m_agentSendTrace(station, agentKey, bytes, startTime, endTime);
}

void
APGenerator::HandleRead(Ptr<Socket> socket)
{
    NS_LOG_FUNCTION(this << socket);
    // Consume incoming data (ACKs) - not used in current design
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

    // Find station from reverse map
    auto it = m_socketToStation.find(&*socket);
    if (it == m_socketToStation.end())
    {
        NS_LOG_ERROR("[AP] Unknown socket in connection succeeded");
        return;
    }
    const Address &station = it->second;
    auto remoteIp = InetSocketAddress::ConvertFrom(station).GetIpv4();

    m_stationConnected[station] = true;
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

    for (const auto& [station, connected] : m_stationConnected)
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

    // Find station from reverse map
    auto it = m_socketToStation.find(&*socket);
    if (it == m_socketToStation.end())
    {
        NS_LOG_ERROR("[AP] Unknown socket in connection failed");
        return;
    }
    const Address &station = it->second;

    m_stationConnected[station] = false;
    NS_FATAL_ERROR("[AP] Failed to connect to station " << station);
}

void
APGenerator::OnCwndChange(uint32_t, uint32_t newCwnd)
{
    NS_LOG_FUNCTION(this << newCwnd);

    const uint32_t second =
      static_cast<uint32_t>(
        std::floor(Simulator::Now().GetSeconds()));
    if (newCwnd == 0)
    {
        return;
    }
    // TODO: on AP side we have X sockets and always rewrite value for same second, to be fixed
    m_perSecondStats[second].lastCwnd = static_cast<double>(newCwnd);
}

void
APGenerator::PrintPerSecondMetrics()
{
    NS_LOG_FUNCTION(this);

    NS_LOG_WARN(
      "========== APGenerator per-second statistics ==========");

    if (m_perSecondStats.empty())
    {
        NS_LOG_WARN("[Final per-second] No transmitted data");
        NS_LOG_WARN(
          "==========================================================");
        return;
    }

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

        for (const auto& [station, stationBytes] :
             stats.stationBytes)
        {
            const double stationThroughputMbps =
              static_cast<double>(stationBytes) * 8.0 / 1e6;

            const double bandwidthSharePercent =
              stats.totalBytes > 0
                ? static_cast<double>(stationBytes) /
                    static_cast<double>(stats.totalBytes) *
                    100.0
                : 0.0;

            NS_LOG_WARN(
              "[Final per-second]   Station "
              << InetSocketAddress::ConvertFrom(station).GetIpv4()
              << ": bytes=" << stationBytes
              << " throughput=" << stationThroughputMbps
              << " Mbps"
              << " share=" << bandwidthSharePercent << "%");
        }
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

    NS_LOG_WARN(
      "[Final overall] duration=" << totalDurationSeconds
      << "s totalBytes=" << totalBytesAllSeconds
      << " avgThroughput=" << avgThroughputMbps << " Mbps");

    NS_LOG_WARN(
      "==========================================================");
}

} // namespace ns3
