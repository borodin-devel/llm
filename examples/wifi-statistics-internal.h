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

/** MAC and PHY observations for one fixed time window. */
struct MacWindowStats
{
    std::map<std::string, uint64_t> upBytes;   ///< Uplink bytes by source station.
    std::map<std::string, uint64_t> downBytes; ///< Downlink bytes by destination station.
    std::map<std::string, PhyRateAccumulator> upPhyRates;   ///< Uplink PHY rates by station.
    std::map<std::string, PhyRateAccumulator> downPhyRates; ///< Downlink PHY rates by station.
};

/** Accumulate delay samples for mean and standard-deviation reporting. */
struct DelayAccumulator
{
    uint64_t count{0};             ///< Number of samples.
    long double sumUs{0.0};        ///< Sum of delays in microseconds.
    long double sumSquaresUs{0.0}; ///< Sum of squared delays in microseconds squared.
    double minUs{std::numeric_limits<double>::infinity()}; ///< Minimum delay in microseconds.
    double maxUs{0.0};                                     ///< Maximum delay in microseconds.

    /** @param delayUs Delay sample in microseconds. */
    void Add(double delayUs)
    {
        ++count;
        sumUs += delayUs;
        sumSquaresUs += static_cast<long double>(delayUs) * delayUs;
        minUs = std::min(minUs, delayUs);
        maxUs = std::max(maxUs, delayUs);
    }

    /** @param other Accumulator to merge. */
    void Merge(const DelayAccumulator& other)
    {
        count += other.count;
        sumUs += other.sumUs;
        sumSquaresUs += other.sumSquaresUs;
        minUs = std::min(minUs, other.minUs);
        maxUs = std::max(maxUs, other.maxUs);
    }

    /** @return Mean delay in microseconds. */
    double MeanUs() const
    {
        return count == 0 ? 0.0 : static_cast<double>(sumUs / count);
    }

    /** @return Population standard deviation in microseconds. */
    double StdDevUs() const
    {
        if (count == 0)
        {
            return 0.0;
        }
        const long double mean = sumUs / count;
        const long double variance = std::max<long double>(0.0, sumSquaresUs / count - mean * mean);
        return std::sqrt(static_cast<double>(variance));
    }
};

/** Application drop totals for one agent. */
struct AgentDropStats
{
    uint64_t events{0}; ///< Drop event count.
    uint64_t bytes{0};  ///< Dropped payload bytes.
};

/** Cross-layer observations for one node and absolute second. */
struct NodeSecondStats
{
    DelayAccumulator appToPhy;                             ///< Application-to-PHY delay samples.
    uint64_t appTxEvents{0};                               ///< Application transmit event count.
    uint64_t appTxBytes{0};                                ///< Application transmit payload bytes.
    uint64_t appDropEvents{0};                             ///< Application drop event count.
    uint64_t appDropBytes{0};                              ///< Application-dropped payload bytes.
    std::map<std::string, AgentDropStats> appDropsByAgent; ///< Drops by agent.
    uint64_t phyTaggedMpduCount{0};                        ///< Tagged PHY MPDU count.
    uint64_t phyPayloadBytes{0};                  ///< Tagged PHY payload bytes including retries.
    uint64_t phyUniquePayloadBytes{0};            ///< Deduplicated tagged PHY payload bytes.
    uint64_t phyMpduBytes{0};                     ///< Complete tagged PHY MPDU bytes.
    uint64_t phyRetransmissions{0};               ///< Tagged PHY retransmission count.
    int64_t phyBusyUs{0};                         ///< PHY busy time in microseconds.
    uint64_t macTxDrops{0};                       ///< MAC transmit-drop event count.
    uint64_t macTxDropBytes{0};                   ///< MAC transmit-drop bytes.
    uint64_t macMpduDrops{0};                     ///< MAC MPDU-drop event count.
    uint64_t macMpduDropBytes{0};                 ///< MAC MPDU-drop bytes.
    std::map<int, uint64_t> macMpduDropsByReason; ///< MAC MPDU drops by reason code.
    uint64_t macDataFailures{0};                  ///< MAC data failure count.
    uint64_t macFinalDataFailures{0};             ///< MAC final data failure count.
};

using PhyMpduKey = std::tuple<uint32_t, std::string, std::string, uint16_t, uint8_t, uint64_t>;

/** All mutable Wi-Fi statistics state for one scenario. */
struct WifiStatisticsState
{
    /** @param trafficCoordinator Traffic epoch and duration owner. */
    explicit WifiStatisticsState(const TrafficCoordinator& trafficCoordinator)
        : coordinator(trafficCoordinator)
    {
    }

    const TrafficCoordinator& coordinator;                 ///< Traffic epoch and duration owner.
    std::vector<std::vector<std::string>> stationIpsByBss; ///< Station IPs by BSS.
    std::map<std::string, int> bssByApIp;                  ///< BSS index by AP IP.
    std::map<std::string, int> bssByStationIp;             ///< BSS index by station IP.
    std::map<uint32_t, std::map<int, MacWindowStats>> macWindows;        ///< MAC windows by node.
    std::map<uint32_t, std::map<int, MacWindowStats>> phyWindows;        ///< PHY windows by node.
    std::map<uint32_t, std::map<uint32_t, NodeSecondStats>> nodeSeconds; ///< Node seconds.
    std::map<uint32_t, std::string> nodeLabels;                          ///< Report label by node.
    std::set<PhyMpduKey> seenTaggedMpdus; ///< Tagged MPDUs already counted as unique.
};

/**
 * Attribute one MAC payload to a fixed statistics window.
 *
 * @param statistics Scenario statistics state.
 * @param windowIndex Zero-based statistics-window index.
 * @param sourceIp Source IPv4 address.
 * @param destinationIp Destination IPv4 address.
 * @param payloadBytes Application payload size in bytes.
 * @return True when the flow belongs to one registered BSS.
 */
bool RecordMacPayloadInWindow(WifiStatisticsState& statistics,
                              uint32_t windowIndex,
                              const std::string& sourceIp,
                              const std::string& destinationIp,
                              uint32_t payloadBytes);

/**
 * Serialize a statistics state to JSON.
 *
 * @param statistics Scenario statistics state.
 * @param outputPath Destination JSON path.
 */
void WriteWifiStatisticsJson(const WifiStatisticsState& statistics, const std::string& outputPath);

} // namespace ns3

#endif // WIFI_STATISTICS_INTERNAL_H
