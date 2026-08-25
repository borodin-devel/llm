#include "experiment-output-internal.h"
#include "scenario-log.h"
#include "traffic-coordinator.h"
#include "wifi-statistics-internal.h"

#include <cmath>
#include <map>
#include <ostream>
#include <string>
#include <vector>

namespace ns3
{

static LogComponent& g_log = llm_example::GetScenarioLog();

static uint64_t
GetMacBytes(const MacWindowStats* statistics, const std::string& stationIpv4, bool uplink)
{
    if (!statistics)
    {
        return 0;
    }
    const auto& bytesByStation = uplink ? statistics->upBytes : statistics->downBytes;
    const auto iterator = bytesByStation.find(stationIpv4);
    return iterator == bytesByStation.end() ? 0 : iterator->second;
}

static const PhyRateAccumulator*
GetPhyRateStats(const MacWindowStats* statistics, const std::string& stationIpv4, bool uplink)
{
    if (!statistics)
    {
        return nullptr;
    }
    const auto& ratesByStation = uplink ? statistics->upPhyRates : statistics->downPhyRates;
    const auto iterator = ratesByStation.find(stationIpv4);
    return iterator == ratesByStation.end() ? nullptr : &iterator->second;
}

static void
WritePhyRateJsonFields(std::ostream& output, const PhyRateAccumulator* rateStatistics)
{
    output << ", \"average_phy_data_rate_mbps\": ";
    if (rateStatistics && rateStatistics->txAttempts > 0)
    {
        WriteJsonScalar(output, rateStatistics->AverageMbps());
    }
    else
    {
        WriteJsonScalar(output, nullptr);
    }
    output << ", \"phy_transmission_attempt_count\": ";
    WriteJsonScalar(output, rateStatistics ? rateStatistics->txAttempts : 0);
    output << ", \"phy_transmission_airtime_us\": ";
    WriteJsonScalar(output, rateStatistics ? rateStatistics->AirtimeUs() : 0.0);
}

struct MacSummaryStats
{
    std::map<std::string, uint64_t> upBytes;
    std::map<std::string, uint64_t> downBytes;
    std::map<std::string, PhyRateAccumulator> upPhyRates;
    std::map<std::string, PhyRateAccumulator> downPhyRates;
    uint64_t upTotalBytes{0};
    uint64_t downTotalBytes{0};
};

static uint64_t
SumMacDirectionBytes(const MacWindowStats* statistics, bool uplink)
{
    if (!statistics)
    {
        return 0;
    }
    const auto& bytesByStation = uplink ? statistics->upBytes : statistics->downBytes;
    uint64_t total = 0;
    for (const auto& [stationIpv4, bytes] : bytesByStation)
    {
        (void)stationIpv4;
        total += bytes;
    }
    return total;
}

static uint64_t
WriteMacFlowArray(std::ostream& output,
                  const WifiStatisticsState& statistics,
                  const std::vector<std::string>& stationIpv4Addresses,
                  const MacWindowStats* windowStatistics,
                  bool uplink,
                  MacSummaryStats& summary,
                  const std::string& indent)
{
    output << '[';
    uint64_t windowTotalPayloadBytes = 0;
    auto& summaryBytesByStation = uplink ? summary.upBytes : summary.downBytes;
    auto& summaryRatesByStation = uplink ? summary.upPhyRates : summary.downPhyRates;
    bool first = true;

    for (const auto& stationIpv4 : stationIpv4Addresses)
    {
        const uint64_t payloadBytes = GetMacBytes(windowStatistics, stationIpv4, uplink);
        if (payloadBytes == 0)
        {
            continue;
        }

        windowTotalPayloadBytes += payloadBytes;
        summaryBytesByStation[stationIpv4] += payloadBytes;
        const PhyRateAccumulator* rateStatistics =
            GetPhyRateStats(windowStatistics, stationIpv4, uplink);
        if (rateStatistics)
        {
            summaryRatesByStation[stationIpv4].Merge(*rateStatistics);
        }
        if (uplink)
        {
            summary.upTotalBytes += payloadBytes;
        }
        else
        {
            summary.downTotalBytes += payloadBytes;
        }

        const double throughputMbps =
            static_cast<double>(payloadBytes) * 8.0 / static_cast<double>(statistics.windowUs);
        output << (first ? "\n" : ",\n") << indent << "{\"station_ipv4\": ";
        WriteJsonScalar(output, stationIpv4);
        output << ", \"payload_bytes\": ";
        WriteJsonScalar(output, payloadBytes);
        output << ", \"throughput_mbps\": ";
        WriteJsonScalar(output, throughputMbps);
        WritePhyRateJsonFields(output, rateStatistics);
        output << '}';
        first = false;
    }

    if (!first)
    {
        output << '\n' << indent.substr(0, indent.size() >= 2 ? indent.size() - 2 : 0);
    }
    output << ']';
    return windowTotalPayloadBytes;
}

static void
WriteMacSummaryFlowArray(std::ostream& output,
                         const std::vector<std::string>& stationIpv4Addresses,
                         const std::map<std::string, uint64_t>& totalBytesByStation,
                         const std::map<std::string, PhyRateAccumulator>& phyRatesByStation,
                         const std::string& indent)
{
    output << '[';
    bool first = true;
    for (const auto& stationIpv4 : stationIpv4Addresses)
    {
        const auto byteIterator = totalBytesByStation.find(stationIpv4);
        if (byteIterator == totalBytesByStation.end() || byteIterator->second == 0)
        {
            continue;
        }

        output << (first ? "\n" : ",\n") << indent << "{\"station_ipv4\": ";
        WriteJsonScalar(output, stationIpv4);
        output << ", \"total_payload_bytes\": ";
        WriteJsonScalar(output, byteIterator->second);
        const auto rateIterator = phyRatesByStation.find(stationIpv4);
        WritePhyRateJsonFields(output,
                               rateIterator == phyRatesByStation.end() ? nullptr
                                                                       : &rateIterator->second);
        output << '}';
        first = false;
    }

    if (!first)
    {
        output << '\n' << indent.substr(0, indent.size() >= 2 ? indent.size() - 2 : 0);
    }
    output << ']';
}

static std::vector<MacSummaryStats>
BuildSparseMacSummary(const WifiStatisticsState& statistics)
{
    std::vector<MacSummaryStats> summary(statistics.stationIpsByBss.size());
    for (const auto& [windowIndex, accessPointStatistics] : statistics.phyWindows)
    {
        (void)windowIndex;
        for (const auto& [accessPointId, windowStatistics] : accessPointStatistics)
        {
            if (accessPointId < 0 ||
                static_cast<std::size_t>(accessPointId) >= statistics.stationIpsByBss.size())
            {
                continue;
            }
            auto& accessPointSummary = summary[accessPointId];
            for (const auto& [stationIpv4, bytes] : windowStatistics.upBytes)
            {
                accessPointSummary.upBytes[stationIpv4] += bytes;
                accessPointSummary.upTotalBytes += bytes;
            }
            for (const auto& [stationIpv4, bytes] : windowStatistics.downBytes)
            {
                accessPointSummary.downBytes[stationIpv4] += bytes;
                accessPointSummary.downTotalBytes += bytes;
            }
        }
    }
    return summary;
}

static bool
MacSummaryEqual(const MacSummaryStats& left, const MacSummaryStats& right)
{
    return left.upTotalBytes == right.upTotalBytes && left.downTotalBytes == right.downTotalBytes &&
           left.upBytes == right.upBytes && left.downBytes == right.downBytes;
}

WifiJsonValidation
WriteWifiStatisticsJsonMembers(std::ostream& output, const WifiStatisticsState& statistics)
{
    const int64_t statisticsDurationUs = static_cast<int64_t>(
        std::ceil(statistics.coordinator.GetMaxExperimentDurationMs() * 1000.0));
    uint64_t windowCount = 0;
    if (statisticsDurationUs > 0)
    {
        const auto durationUs = static_cast<uint64_t>(statisticsDurationUs);
        const auto windowUs = static_cast<uint64_t>(statistics.windowUs);
        windowCount = durationUs / windowUs + (durationUs % windowUs != 0);
    }

    std::vector<MacSummaryStats> summaryFromWindows(statistics.stationIpsByBss.size());
    WifiJsonValidation validation;
    output << "  \"statistics_window_ms\": ";
    WriteJsonScalar(output, statistics.windowMs);
    output << ",\n  \"wifi_windows\": [\n";

    bool firstWindow = true;
    for (const auto& [windowIndex, accessPointStatistics] : statistics.phyWindows)
    {
        if (windowIndex >= windowCount)
        {
            NS_LOG_ERROR("[MAC stats] bucket outside configured experiment range: bucket="
                         << windowIndex << " windowCount=" << windowCount);
            validation.windowPayloadTotalsConsistent = false;
            continue;
        }

        const uint64_t windowEndMs = (windowIndex + 1) * static_cast<uint64_t>(statistics.windowMs);
        output << (firstWindow ? "" : ",\n") << "    {\n      \"window_end_ms\": ";
        WriteJsonScalar(output, windowEndMs);
        output << ",\n      \"access_points\": [\n";
        firstWindow = false;
        bool firstAccessPoint = true;

        for (const auto& [accessPointIdValue, windowStatisticsValue] : accessPointStatistics)
        {
            if (accessPointIdValue < 0 ||
                static_cast<std::size_t>(accessPointIdValue) >= statistics.stationIpsByBss.size())
            {
                NS_LOG_ERROR("[MAC stats] invalid AP id in bucket=" << windowIndex << " AP="
                                                                    << accessPointIdValue);
                validation.windowPayloadTotalsConsistent = false;
                continue;
            }

            const std::size_t accessPointId = static_cast<std::size_t>(accessPointIdValue);
            const auto* windowStatistics = &windowStatisticsValue;
            const uint64_t sparseUplinkTotal = SumMacDirectionBytes(windowStatistics, true);
            const uint64_t sparseDownlinkTotal = SumMacDirectionBytes(windowStatistics, false);
            if (sparseUplinkTotal == 0 && sparseDownlinkTotal == 0)
            {
                continue;
            }

            output << (firstAccessPoint ? "" : ",\n")
                   << "        {\n          \"access_point_id\": ";
            WriteJsonScalar(output, accessPointId);
            output << ",\n          \"uplink\": {\n"
                   << "            \"total_payload_bytes\": ";
            WriteJsonScalar(output, sparseUplinkTotal);
            output << ",\n            \"flows\": ";
            const uint64_t emittedUplinkTotal =
                WriteMacFlowArray(output,
                                  statistics,
                                  statistics.stationIpsByBss[accessPointId],
                                  windowStatistics,
                                  true,
                                  summaryFromWindows[accessPointId],
                                  "              ");
            output << "\n          },\n          \"downlink\": {\n"
                   << "            \"total_payload_bytes\": ";
            WriteJsonScalar(output, sparseDownlinkTotal);
            output << ",\n            \"flows\": ";
            const uint64_t emittedDownlinkTotal =
                WriteMacFlowArray(output,
                                  statistics,
                                  statistics.stationIpsByBss[accessPointId],
                                  windowStatistics,
                                  false,
                                  summaryFromWindows[accessPointId],
                                  "              ");
            output << "\n          }\n        }";
            firstAccessPoint = false;

            if (emittedUplinkTotal != sparseUplinkTotal)
            {
                validation.windowPayloadTotalsConsistent = false;
                NS_LOG_ERROR("[MAC stats] UL window total mismatch: bucket="
                             << windowIndex << " AP=" << accessPointId << " emitted="
                             << emittedUplinkTotal << " sparse=" << sparseUplinkTotal);
            }
            if (emittedDownlinkTotal != sparseDownlinkTotal)
            {
                validation.windowPayloadTotalsConsistent = false;
                NS_LOG_ERROR("[MAC stats] DL window total mismatch: bucket="
                             << windowIndex << " AP=" << accessPointId << " emitted="
                             << emittedDownlinkTotal << " sparse=" << sparseDownlinkTotal);
            }
        }
        output << "\n      ]\n    }";
    }
    if (!firstWindow)
    {
        output << '\n';
    }

    const std::vector<MacSummaryStats> summaryFromSparse = BuildSparseMacSummary(statistics);
    validation.summaryPayloadTotalsConsistent =
        summaryFromWindows.size() == summaryFromSparse.size();
    if (validation.summaryPayloadTotalsConsistent)
    {
        for (std::size_t accessPointId = 0; accessPointId < summaryFromWindows.size();
             ++accessPointId)
        {
            if (!MacSummaryEqual(summaryFromWindows[accessPointId],
                                 summaryFromSparse[accessPointId]))
            {
                validation.summaryPayloadTotalsConsistent = false;
                NS_LOG_ERROR("[MAC stats] summary mismatch for AP " << accessPointId);
            }
        }
    }

    output << "  ],\n  \"wifi_summary\": [\n";
    for (std::size_t accessPointId = 0; accessPointId < summaryFromWindows.size(); ++accessPointId)
    {
        const auto& summary = summaryFromWindows[accessPointId];
        output << "    {\n      \"access_point_id\": ";
        WriteJsonScalar(output, accessPointId);
        output << ",\n      \"uplink\": {\n        \"total_payload_bytes\": ";
        WriteJsonScalar(output, summary.upTotalBytes);
        output << ",\n        \"flows\": ";
        WriteMacSummaryFlowArray(output,
                                 statistics.stationIpsByBss[accessPointId],
                                 summary.upBytes,
                                 summary.upPhyRates,
                                 "          ");
        output << "\n      },\n      \"downlink\": {\n        \"total_payload_bytes\": ";
        WriteJsonScalar(output, summary.downTotalBytes);
        output << ",\n        \"flows\": ";
        WriteMacSummaryFlowArray(output,
                                 statistics.stationIpsByBss[accessPointId],
                                 summary.downBytes,
                                 summary.downPhyRates,
                                 "          ");
        output << "\n      }\n    }";
        if (accessPointId + 1 < summaryFromWindows.size())
        {
            output << ',';
        }
        output << '\n';
    }
    output << "  ]";
    return validation;
}

} // namespace ns3
