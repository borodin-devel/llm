#ifndef WIFI_STATISTICS_INTERNAL_H
#define WIFI_STATISTICS_INTERNAL_H

#include "wifi-statistics.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace ns3
{

class TrafficCoordinator;

struct MacWindowStats
{
    std::map<std::string, uint64_t> upBytes;
    std::map<std::string, uint64_t> downBytes;
    std::map<std::string, PhyRateAccumulator> upPhyRates;
    std::map<std::string, PhyRateAccumulator> downPhyRates;
};

struct DelayAccumulator
{
    uint64_t count{0};
    long double sumUs{0.0};
    long double sumSquaresUs{0.0};
    double minUs{std::numeric_limits<double>::infinity()};
    double maxUs{0.0};

    void Add(double delayUs)
    {
        ++count;
        sumUs += delayUs;
        sumSquaresUs += static_cast<long double>(delayUs) * delayUs;
        minUs = std::min(minUs, delayUs);
        maxUs = std::max(maxUs, delayUs);
    }

    void Merge(const DelayAccumulator& other)
    {
        count += other.count;
        sumUs += other.sumUs;
        sumSquaresUs += other.sumSquaresUs;
        minUs = std::min(minUs, other.minUs);
        maxUs = std::max(maxUs, other.maxUs);
    }

    double MeanUs() const
    {
        return count == 0 ? 0.0 : static_cast<double>(sumUs / count);
    }

    double StdDevUs() const
    {
        if (count == 0)
        {
            return 0.0;
        }
        const long double mean = sumUs / count;
        const long double variance =
            std::max<long double>(0.0, sumSquaresUs / count - mean * mean);
        return std::sqrt(static_cast<double>(variance));
    }
};

struct AgentDropStats
{
    uint64_t events{0};
    uint64_t bytes{0};
};

struct NodeSecondStats
{
    DelayAccumulator appToPhy;
    uint64_t appTxEvents{0};
    uint64_t appTxBytes{0};
    uint64_t appDropEvents{0};
    uint64_t appDropBytes{0};
    std::map<std::string, AgentDropStats> appDropsByAgent;
    uint64_t phyTaggedMpduCount{0};
    uint64_t phyPayloadBytes{0};
    uint64_t phyUniquePayloadBytes{0};
    uint64_t phyMpduBytes{0};
    uint64_t phyRetransmissions{0};
    int64_t phyBusyUs{0};
    uint64_t macTxDrops{0};
    uint64_t macTxDropBytes{0};
    uint64_t macMpduDrops{0};
    uint64_t macMpduDropBytes{0};
    std::map<int, uint64_t> macMpduDropsByReason;
    uint64_t macDataFailures{0};
    uint64_t macFinalDataFailures{0};
};

using PhyMpduKey =
    std::tuple<uint32_t, std::string, std::string, uint16_t, uint8_t, uint64_t>;

struct WifiStatisticsState
{
    explicit WifiStatisticsState(const TrafficCoordinator& trafficCoordinator)
        : coordinator(trafficCoordinator)
    {
    }

    const TrafficCoordinator& coordinator;
    std::vector<std::vector<std::string>> stationIpsByBss;
    std::map<std::string, int> bssByApIp;
    std::map<std::string, int> bssByStationIp;
    std::map<uint32_t, std::map<int, MacWindowStats>> macWindows;
    std::map<uint32_t, std::map<int, MacWindowStats>> phyWindows;
    std::map<uint32_t, std::map<uint32_t, NodeSecondStats>> nodeSeconds;
    std::map<uint32_t, std::string> nodeLabels;
    std::set<PhyMpduKey> seenTaggedMpdus;
};

WifiStatisticsState& GetWifiStatisticsState();

} // namespace ns3

#endif // WIFI_STATISTICS_INTERNAL_H
