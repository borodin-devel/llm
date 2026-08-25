#include "traffic-coordinator.h"
#include "wifi-statistics-internal.h"
#include "wifi-statistics.h"

#include "ns3/inet-socket-address.h"
#include "ns3/nstime.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <sstream>
#include <tuple>

namespace ns3
{

bool
TcpConnectionKey::operator<(const TcpConnectionKey& other) const
{
    return std::tie(ownerNodeId, direction, peerNodeId) <
           std::tie(other.ownerNodeId, other.direction, other.peerNodeId);
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

int64_t
GetExperimentDurationUs(const WifiStatisticsState& statistics)
{
    return ConvertExperimentDurationMsToUs(statistics.coordinator.GetMaxExperimentDurationMs());
}

void
IntegrateCongestionWindow(WifiStatisticsState& statistics,
                          const TcpConnectionKey& key,
                          TcpConnectionState& state,
                          int64_t absoluteEndUs)
{
    if (!state.currentCongestionWindowBytes || statistics.coordinator.GetExperimentStartUs() < 0)
    {
        return;
    }

    const int64_t epochUs = statistics.coordinator.GetExperimentStartUs();
    const int64_t durationUs = GetExperimentDurationUs(statistics);
    const int64_t experimentEndUs = epochUs + durationUs;
    const int64_t startUs = std::max(state.stateStartUs, epochUs);
    const int64_t endUs = std::min(absoluteEndUs, experimentEndUs);
    if (endUs <= startUs)
    {
        return;
    }

    const auto overlaps = SplitExperimentInterval(startUs - epochUs,
                                                  endUs - epochUs,
                                                  durationUs,
                                                  statistics.windowUs);
    for (const auto& [windowIndex, overlapUs] : overlaps)
    {
        auto& accumulator = statistics.unifiedWindows[windowIndex][key.ownerNodeId]
                                .tcpConnections[{key.direction, key.peerNodeId}];
        accumulator.congestionWindowBytesUs +=
            static_cast<long double>(*state.currentCongestionWindowBytes) * overlapUs;
        accumulator.congestionWindowObservationDurationUs += static_cast<uint64_t>(overlapUs);
        accumulator.lastCongestionWindowBytes = state.currentCongestionWindowBytes;
    }
}

} // namespace

void
WifiStatistics::RecordCongestionWindow(uint32_t ownerNodeId,
                                       ExperimentDirection direction,
                                       uint32_t peerNodeId,
                                       uint32_t newCwndBytes,
                                       int64_t absoluteTimeUs)
{
    if (m_state->tcpStatisticsFinalized)
    {
        return;
    }

    const TcpConnectionKey key{ownerNodeId, direction, peerNodeId};
    auto& state = m_state->tcpConnectionStates[key];
    IntegrateCongestionWindow(*m_state, key, state, absoluteTimeUs);
    state.currentCongestionWindowBytes = newCwndBytes;
    state.stateStartUs = absoluteTimeUs;
}

void
WifiStatistics::RecordRoundTripTime(uint32_t ownerNodeId,
                                    ExperimentDirection direction,
                                    uint32_t peerNodeId,
                                    int64_t rttUs,
                                    int64_t absoluteTimeUs)
{
    if (m_state->tcpStatisticsFinalized || rttUs <= 0)
    {
        return;
    }

    uint64_t windowIndex = 0;
    if (!GetStatisticsWindowIndex(absoluteTimeUs,
                                  m_state->coordinator.GetExperimentStartUs(),
                                  m_state->coordinator.GetMaxExperimentDurationMs(),
                                  m_state->windowMs,
                                  windowIndex))
    {
        return;
    }

    m_state->unifiedWindows[windowIndex][ownerNodeId]
        .tcpConnections[{direction, peerNodeId}]
        .roundTripTimeUs.Add(static_cast<double>(rttUs));
}

void
WifiStatistics::FinalizeTcpStatistics()
{
    if (m_state->tcpStatisticsFinalized)
    {
        return;
    }

    const int64_t experimentEndUs =
        m_state->coordinator.GetExperimentStartUs() + GetExperimentDurationUs(*m_state);
    for (auto& [key, state] : m_state->tcpConnectionStates)
    {
        IntegrateCongestionWindow(*m_state, key, state, experimentEndUs);
        state.stateStartUs = experimentEndUs;
    }
    m_state->tcpStatisticsFinalized = true;
}

void
WifiStatistics::RecordApCongestionWindow(uint32_t nodeId,
                                         Address peer,
                                         uint32_t newCwndBytes,
                                         Time eventTime)
{
    const auto peerNodeId = ResolvePeerNodeId(*m_state, peer);
    if (peerNodeId)
    {
        RecordCongestionWindow(nodeId,
                               ExperimentDirection::DOWNLINK,
                               *peerNodeId,
                               newCwndBytes,
                               eventTime.GetMicroSeconds());
    }
}

void
WifiStatistics::RecordApRoundTripTime(uint32_t nodeId, Address peer, Time rtt, Time eventTime)
{
    const auto peerNodeId = ResolvePeerNodeId(*m_state, peer);
    if (peerNodeId)
    {
        RecordRoundTripTime(nodeId,
                            ExperimentDirection::DOWNLINK,
                            *peerNodeId,
                            rtt.GetMicroSeconds(),
                            eventTime.GetMicroSeconds());
    }
}

void
WifiStatistics::RecordStaCongestionWindow(uint32_t nodeId,
                                          Address peer,
                                          uint32_t newCwndBytes,
                                          Time eventTime)
{
    const auto peerNodeId = ResolvePeerNodeId(*m_state, peer);
    if (peerNodeId)
    {
        RecordCongestionWindow(nodeId,
                               ExperimentDirection::UPLINK,
                               *peerNodeId,
                               newCwndBytes,
                               eventTime.GetMicroSeconds());
    }
}

void
WifiStatistics::RecordStaRoundTripTime(uint32_t nodeId, Address peer, Time rtt, Time eventTime)
{
    const auto peerNodeId = ResolvePeerNodeId(*m_state, peer);
    if (peerNodeId)
    {
        RecordRoundTripTime(nodeId,
                            ExperimentDirection::UPLINK,
                            *peerNodeId,
                            rtt.GetMicroSeconds(),
                            eventTime.GetMicroSeconds());
    }
}

} // namespace ns3
