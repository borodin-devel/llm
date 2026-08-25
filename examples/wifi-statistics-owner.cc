#include "traffic-coordinator.h"
#include "wifi-statistics-internal.h"
#include "wifi-statistics.h"

#include "ns3/core-module.h"
#include "ns3/internet-module.h"

#include <cmath>
#include <sstream>

namespace ns3
{

static std::string
Ipv4ToString(Ipv4Address address)
{
    std::ostringstream stream;
    address.Print(stream);
    return stream.str();
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
                         uint64_t& windowIndex)
{
    if (experimentStartUs < 0 || absoluteUs < experimentStartUs || windowMs == 0)
    {
        return false;
    }

    const int64_t relativeUs = absoluteUs - experimentStartUs;
    const int64_t experimentDurationUs = ConvertExperimentDurationMsToUs(maxExperimentDurationMs);
    if (relativeUs >= experimentDurationUs)
    {
        return false;
    }

    const int64_t windowUs = static_cast<int64_t>(windowMs) * 1000;
    windowIndex = static_cast<uint64_t>(relativeUs / windowUs);
    return true;
}

WifiStatistics::WifiStatistics(const TrafficCoordinator& coordinator, uint32_t windowMs)
    : m_state(std::make_unique<WifiStatisticsState>(coordinator, windowMs))
{
}

WifiStatistics::~WifiStatistics() = default;

bool
RecordMacPayloadInWindow(WifiStatisticsState& statistics,
                         uint64_t windowIndex,
                         const std::string& sourceIp,
                         const std::string& destinationIp,
                         uint32_t payloadBytes)
{
    if (payloadBytes == 0)
    {
        return false;
    }

    const auto sourceStation = statistics.bssByStationIp.find(sourceIp);
    const auto destinationAp = statistics.bssByApIp.find(destinationIp);
    if (sourceStation != statistics.bssByStationIp.end() &&
        destinationAp != statistics.bssByApIp.end() &&
        sourceStation->second == destinationAp->second)
    {
        statistics.macWindows[windowIndex][sourceStation->second].upBytes[sourceIp] += payloadBytes;
        return true;
    }

    const auto sourceAp = statistics.bssByApIp.find(sourceIp);
    const auto destinationStation = statistics.bssByStationIp.find(destinationIp);
    if (sourceAp != statistics.bssByApIp.end() &&
        destinationStation != statistics.bssByStationIp.end() &&
        sourceAp->second == destinationStation->second)
    {
        statistics.macWindows[windowIndex][sourceAp->second].downBytes[destinationIp] +=
            payloadBytes;
        return true;
    }

    return false;
}

void
WifiStatistics::RegisterApGroup(int bssIndex,
                                Ipv4Address apAddress,
                                const Ipv4InterfaceContainer& stationInterfaces)
{
    auto& state = *m_state;
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
WifiStatistics::RegisterAccessPointIdentity(uint32_t accessPointId,
                                            uint32_t nodeId,
                                            std::string nodeLabel,
                                            Ipv4Address ipv4)
{
    m_state->entityRegistry.RegisterAccessPoint(accessPointId,
                                                nodeId,
                                                std::move(nodeLabel),
                                                Ipv4ToString(ipv4));
}

void
WifiStatistics::RegisterStationIdentity(uint32_t accessPointId,
                                        uint32_t stationIndex,
                                        uint32_t nodeId,
                                        std::string nodeLabel,
                                        Ipv4Address ipv4)
{
    m_state->entityRegistry.RegisterStation(accessPointId,
                                            stationIndex,
                                            nodeId,
                                            std::move(nodeLabel),
                                            Ipv4ToString(ipv4));
}

void
WifiStatistics::RecordMacPayload(int64_t nowUs,
                                 const std::string& sourceIp,
                                 const std::string& destinationIp,
                                 uint32_t payloadBytes)
{
    auto& state = *m_state;
    if (state.coordinator.GetExperimentStartUs() < 0 ||
        nowUs < state.coordinator.GetExperimentStartUs() || payloadBytes == 0)
    {
        return;
    }

    const int64_t relativeUs = nowUs - state.coordinator.GetExperimentStartUs();
    const int64_t statsEndUs =
        ConvertExperimentDurationMsToUs(state.coordinator.GetMaxExperimentDurationMs());
    if (relativeUs >= statsEndUs)
    {
        return;
    }
    const uint64_t windowIndex = static_cast<uint64_t>(relativeUs / state.windowUs);

    RecordMacPayloadInWindow(state, windowIndex, sourceIp, destinationIp, payloadBytes);
}

} // namespace ns3
