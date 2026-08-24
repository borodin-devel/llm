#include "wifi-statistics.h"

#include "traffic-coordinator.h"
#include "wifi-statistics-internal.h"

#include "ns3/core-module.h"
#include "ns3/internet-module.h"

#include <cmath>
#include <sstream>

namespace ns3
{

static constexpr int64_t kMacStatsWindowUs = 10000;
static WifiStatisticsState* g_activeStatistics = nullptr;

WifiStatisticsState&
GetWifiStatisticsState()
{
    NS_ABORT_MSG_IF(!g_activeStatistics, "Wi-Fi statistics owner is not active");
    return *g_activeStatistics;
}

void
PhyRateAccumulator::Add(double rateBps, double allocatedAirtimeUs)
{
    if (rateBps <= 0.0 || allocatedAirtimeUs <= 0.0)
    {
        return;
    }
    ++txAttempts;
    weightedRateBpsUs += static_cast<long double>(rateBps) * allocatedAirtimeUs;
    airtimeUs += allocatedAirtimeUs;
}

void
PhyRateAccumulator::Merge(const PhyRateAccumulator& other)
{
    txAttempts += other.txAttempts;
    weightedRateBpsUs += other.weightedRateBpsUs;
    airtimeUs += other.airtimeUs;
}

double
PhyRateAccumulator::AverageMbps() const
{
    return airtimeUs > 0.0 ? static_cast<double>(weightedRateBpsUs / airtimeUs / 1e6L) : 0.0;
}

double
PhyRateAccumulator::AirtimeUs() const
{
    return static_cast<double>(airtimeUs);
}

bool
GetStatisticsWindowIndex(int64_t absoluteUs,
                         int64_t experimentStartUs,
                         double maxExperimentDurationMs,
                         uint32_t windowMs,
                         uint32_t& windowIndex)
{
    if (experimentStartUs < 0 || absoluteUs < experimentStartUs || windowMs == 0)
    {
        return false;
    }

    const int64_t relativeUs = absoluteUs - experimentStartUs;
    const int64_t experimentDurationUs =
        static_cast<int64_t>(std::ceil(maxExperimentDurationMs * 1000.0));
    if (relativeUs >= experimentDurationUs)
    {
        return false;
    }

    const int64_t windowUs = static_cast<int64_t>(windowMs) * 1000;
    windowIndex = static_cast<uint32_t>(relativeUs / windowUs);
    return true;
}

WifiStatistics::WifiStatistics(const TrafficCoordinator& coordinator)
    : m_state(std::make_unique<WifiStatisticsState>(coordinator))
{
    NS_ABORT_MSG_IF(g_activeStatistics, "Only one Wi-Fi statistics owner may be active");
    g_activeStatistics = m_state.get();
}

WifiStatistics::~WifiStatistics()
{
    if (g_activeStatistics == m_state.get())
    {
        g_activeStatistics = nullptr;
    }
}

void
WifiStatistics::RegisterApGroup(int bssIndex,
                                Ipv4Address apAddress,
                                const Ipv4InterfaceContainer& stationInterfaces)
{
    auto& state = GetWifiStatisticsState();
    if (state.stationIpsByBss.size() <= static_cast<std::size_t>(bssIndex))
    {
        state.stationIpsByBss.resize(bssIndex + 1);
    }

    std::ostringstream apStream;
    apAddress.Print(apStream);
    state.bssByApIp[apStream.str()] = bssIndex;

    auto& stationIps = state.stationIpsByBss[bssIndex];
    stationIps.clear();
    stationIps.reserve(stationInterfaces.GetN());
    for (uint32_t stationIndex = 0; stationIndex < stationInterfaces.GetN(); ++stationIndex)
    {
        std::ostringstream stationStream;
        stationInterfaces.GetAddress(stationIndex).Print(stationStream);
        stationIps.push_back(stationStream.str());
        state.bssByStationIp[stationStream.str()] = bssIndex;
    }
}

void
WifiStatistics::RecordMacPayload(int64_t nowUs,
                                 const std::string& sourceIp,
                                 const std::string& destinationIp,
                                 uint32_t payloadBytes)
{
    auto& state = GetWifiStatisticsState();
    if (state.coordinator.GetExperimentStartUs() < 0 ||
        nowUs < state.coordinator.GetExperimentStartUs() || payloadBytes == 0)
    {
        return;
    }

    const int64_t relativeUs = nowUs - state.coordinator.GetExperimentStartUs();
    const int64_t statsEndUs = static_cast<int64_t>(
        std::ceil(state.coordinator.GetMaxExperimentDurationMs() * 1000.0));
    if (relativeUs >= statsEndUs)
    {
        return;
    }
    const uint32_t windowIndex = static_cast<uint32_t>(relativeUs / kMacStatsWindowUs);

    const auto sourceStation = state.bssByStationIp.find(sourceIp);
    const auto destinationAp = state.bssByApIp.find(destinationIp);
    if (sourceStation != state.bssByStationIp.end() &&
        destinationAp != state.bssByApIp.end() &&
        sourceStation->second == destinationAp->second)
    {
        state.macWindows[windowIndex][sourceStation->second].upBytes[sourceIp] += payloadBytes;
        return;
    }

    const auto sourceAp = state.bssByApIp.find(sourceIp);
    const auto destinationStation = state.bssByStationIp.find(destinationIp);
    if (sourceAp != state.bssByApIp.end() &&
        destinationStation != state.bssByStationIp.end() &&
        sourceAp->second == destinationStation->second)
    {
        state.macWindows[windowIndex][sourceAp->second].downBytes[destinationIp] += payloadBytes;
    }
}

} // namespace ns3
