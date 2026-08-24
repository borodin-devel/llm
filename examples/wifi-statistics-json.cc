#include "scenario-log.h"
#include "traffic-coordinator.h"
#include "wifi-statistics-internal.h"
#include "wifi-statistics.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <map>
#include <string>
#include <vector>

namespace ns3
{

static LogComponent& g_log = llm_example::GetScenarioLog();
static constexpr uint32_t kMacStatsWindowMs = 10;
static constexpr int64_t kMacStatsWindowUs = static_cast<int64_t>(kMacStatsWindowMs) * 1000;

static uint64_t
GetMacBytes(const MacWindowStats* stats, const std::string& staIp, bool uplink)
{
    if (!stats)
    {
        return 0;
    }

    const auto& bytesBySta = uplink ? stats->upBytes : stats->downBytes;
    auto it = bytesBySta.find(staIp);
    return it == bytesBySta.end() ? 0 : it->second;
}

static const PhyRateAccumulator*
GetPhyRateStats(const MacWindowStats* stats, const std::string& staIp, bool uplink)
{
    if (!stats)
    {
        return nullptr;
    }

    const auto& ratesBySta = uplink ? stats->upPhyRates : stats->downPhyRates;
    const auto it = ratesBySta.find(staIp);
    return it == ratesBySta.end() ? nullptr : &it->second;
}

static void
WritePhyRateJsonFields(std::ofstream& out, const PhyRateAccumulator* rateStats)
{
    out << ", \"avg_phy_data_rate_mbps\": ";
    if (rateStats && rateStats->txAttempts > 0)
    {
        out << std::fixed << std::setprecision(6) << rateStats->AverageMbps();
    }
    else
    {
        out << "null";
    }

    out << ", \"phy_tx_attempts\": " << (rateStats ? rateStats->txAttempts : 0)
        << ", \"phy_tx_airtime_us\": " << std::fixed << std::setprecision(3)
        << (rateStats ? rateStats->AirtimeUs() : 0.0);
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
SumMacDirectionBytes(const MacWindowStats* stats, bool uplink)
{
    if (!stats)
    {
        return 0;
    }

    const auto& bytesBySta = uplink ? stats->upBytes : stats->downBytes;
    uint64_t total = 0;

    for (const auto& [staIp, bytes] : bytesBySta)
    {
        (void)staIp;
        total += bytes;
    }

    return total;
}

static uint64_t
WriteMacFlowArray(std::ofstream& out,
                  const std::vector<std::string>& staIps,
                  const MacWindowStats* stats,
                  bool uplink,
                  MacSummaryStats& summary,
                  const std::string& indent)
{
    out << "[";

    uint64_t windowTotalBytes = 0;
    auto& summaryBytesBySta = uplink ? summary.upBytes : summary.downBytes;
    auto& summaryRatesBySta = uplink ? summary.upPhyRates : summary.downPhyRates;
    bool first = true;

    for (const auto& staIp : staIps)
    {
        const uint64_t bytes = GetMacBytes(stats, staIp, uplink);
        if (bytes == 0)
        {
            continue;
        }

        windowTotalBytes += bytes;
        summaryBytesBySta[staIp] += bytes;

        const PhyRateAccumulator* rateStats = GetPhyRateStats(stats, staIp, uplink);
        if (rateStats)
        {
            summaryRatesBySta[staIp].Merge(*rateStats);
        }

        if (uplink)
        {
            summary.upTotalBytes += bytes;
        }
        else
        {
            summary.downTotalBytes += bytes;
        }

        // Mbps for this fixed window:
        // bytes * 8 / (window_seconds * 1e6).
        // Since window_seconds = window_us / 1e6, this simplifies to
        // bytes * 8 / window_us.
        const double bwMbps =
            static_cast<double>(bytes) * 8.0 / static_cast<double>(kMacStatsWindowUs);

        if (first)
        {
            out << "\n";
            first = false;
        }
        else
        {
            out << ",\n";
        }

        out << indent << "{\"host_id\": \"" << staIp << "\", \"bytes\": " << bytes
            << ", \"bw\": " << std::fixed << std::setprecision(6) << bwMbps;
        WritePhyRateJsonFields(out, rateStats);
        out << "}";
    }

    if (!first)
    {
        out << "\n" << indent.substr(0, indent.size() >= 2 ? indent.size() - 2 : 0);
    }
    out << "]";
    return windowTotalBytes;
}

static void
WriteMacSummaryFlowArray(std::ofstream& out,
                         const std::vector<std::string>& staIps,
                         const std::map<std::string, uint64_t>& totalBytesBySta,
                         const std::map<std::string, PhyRateAccumulator>& phyRatesBySta,
                         const std::string& indent)
{
    out << "[";
    bool first = true;

    for (const auto& staIp : staIps)
    {
        auto it = totalBytesBySta.find(staIp);
        if (it == totalBytesBySta.end() || it->second == 0)
        {
            continue;
        }

        if (first)
        {
            out << "\n";
            first = false;
        }
        else
        {
            out << ",\n";
        }

        out << indent << "{\"host_id\": \"" << staIp << "\", \"total_bytes\": " << it->second;

        const auto rateIt = phyRatesBySta.find(staIp);
        WritePhyRateJsonFields(out, rateIt == phyRatesBySta.end() ? nullptr : &rateIt->second);
        out << "}";
    }

    if (!first)
    {
        out << "\n" << indent.substr(0, indent.size() >= 2 ? indent.size() - 2 : 0);
    }
    out << "]";
}

static std::vector<MacSummaryStats>
BuildSparseMacSummary(const WifiStatisticsState& statistics)
{
    std::vector<MacSummaryStats> summary(statistics.stationIpsByBss.size());

    for (const auto& [bucketIndex, apStats] : statistics.phyWindows)
    {
        (void)bucketIndex;

        for (const auto& [apId, stats] : apStats)
        {
            if (apId < 0 || static_cast<std::size_t>(apId) >= summary.size())
            {
                continue;
            }

            auto& apSummary = summary[apId];

            for (const auto& [staIp, bytes] : stats.upBytes)
            {
                apSummary.upBytes[staIp] += bytes;
                apSummary.upTotalBytes += bytes;
            }

            for (const auto& [staIp, bytes] : stats.downBytes)
            {
                apSummary.downBytes[staIp] += bytes;
                apSummary.downTotalBytes += bytes;
            }
        }
    }

    return summary;
}

static bool
MacSummaryEqual(const MacSummaryStats& lhs, const MacSummaryStats& rhs)
{
    return lhs.upTotalBytes == rhs.upTotalBytes && lhs.downTotalBytes == rhs.downTotalBytes &&
           lhs.upBytes == rhs.upBytes && lhs.downBytes == rhs.downBytes;
}

void
WriteWifiStatisticsJson(const WifiStatisticsState& statistics, const std::string& outputPath)
{
    std::ofstream out(outputPath);
    if (!out)
    {
        NS_LOG_ERROR("Cannot open PHY statistics output: " << outputPath);
        return;
    }

    const int64_t statsDurationUs = static_cast<int64_t>(
        std::ceil(statistics.coordinator.GetMaxExperimentDurationMs() * 1000.0));

    const uint32_t windowCount =
        statsDurationUs > 0
            ? static_cast<uint32_t>((statsDurationUs + kMacStatsWindowUs - 1) / kMacStatsWindowUs)
            : 0;

    std::vector<MacSummaryStats> summaryFromWindows(statistics.stationIpsByBss.size());
    bool windowTotalsConsistent = true;

    out << "{\n"
        << "  \"source\": \"PhyTxBegin+PhyTxPsduBegin/AppTxTag\",\n"
        << "  \"byte_semantics\": \"tagged application payload observed at PHY; retransmissions "
           "included\",\n"
        << "  \"phy_rate_semantics\": \"airtime-weighted nominal WifiTxVector data rate of actual "
           "tagged PPDU attempts; retransmissions included; PPDU airtime allocated by tagged "
           "payload bytes\",\n"
        << "  \"window_ms\": " << kMacStatsWindowMs << ",\n"
        << "  \"windows\": [\n";

    bool firstWindow = true;
    uint32_t emittedWindowCount = 0;

    // The window map is sparse; absent windows and flows represent zero traffic.
    for (const auto& [bucketIndex, apStats] : statistics.phyWindows)
    {
        if (bucketIndex >= windowCount)
        {
            NS_LOG_ERROR("[MAC stats] bucket outside configured experiment range: bucket="
                         << bucketIndex << " windowCount=" << windowCount);
            windowTotalsConsistent = false;
            continue;
        }

        const uint32_t timestampMs = (bucketIndex + 1) * kMacStatsWindowMs;

        if (!firstWindow)
        {
            out << ",\n";
        }
        firstWindow = false;
        ++emittedWindowCount;

        out << "    {\n"
            << "      \"timestamp\": " << timestampMs << ",\n"
            << "      \"stats\": [\n";

        bool firstAp = true;

        for (const auto& [apIdInt, statsValue] : apStats)
        {
            if (apIdInt < 0 ||
                static_cast<std::size_t>(apIdInt) >= statistics.stationIpsByBss.size())
            {
                NS_LOG_ERROR("[MAC stats] invalid AP id in bucket=" << bucketIndex
                                                                    << " AP=" << apIdInt);
                windowTotalsConsistent = false;
                continue;
            }

            const std::size_t apId = static_cast<std::size_t>(apIdInt);
            const MacWindowStats* stats = &statsValue;

            const uint64_t sparseUpTotal = SumMacDirectionBytes(stats, true);
            const uint64_t sparseDownTotal = SumMacDirectionBytes(stats, false);

            // Defensive guard: RecordMacStats only inserts positive payloads,
            // but do not serialize an empty AP object if that invariant changes.
            if (sparseUpTotal == 0 && sparseDownTotal == 0)
            {
                continue;
            }

            if (!firstAp)
            {
                out << ",\n";
            }
            firstAp = false;

            out << "        {\n"
                << "          \"ap_id\": " << apId << ",\n"
                << "          \"up_flows\": ";

            const uint64_t upTotalBytes = WriteMacFlowArray(out,
                                                            statistics.stationIpsByBss[apId],
                                                            stats,
                                                            true,
                                                            summaryFromWindows[apId],
                                                            "            ");

            if (upTotalBytes != sparseUpTotal)
            {
                windowTotalsConsistent = false;
                NS_LOG_ERROR("[MAC stats] UL window total mismatch: bucket="
                             << bucketIndex << " AP=" << apId << " emitted=" << upTotalBytes
                             << " sparse=" << sparseUpTotal);
            }

            out << ",\n"
                << "          \"up_total_bytes\": " << upTotalBytes << ",\n"
                << "          \"down_flows\": ";

            const uint64_t downTotalBytes = WriteMacFlowArray(out,
                                                              statistics.stationIpsByBss[apId],
                                                              stats,
                                                              false,
                                                              summaryFromWindows[apId],
                                                              "            ");

            if (downTotalBytes != sparseDownTotal)
            {
                windowTotalsConsistent = false;
                NS_LOG_ERROR("[MAC stats] DL window total mismatch: bucket="
                             << bucketIndex << " AP=" << apId << " emitted=" << downTotalBytes
                             << " sparse=" << sparseDownTotal);
            }

            out << ",\n"
                << "          \"down_total_bytes\": " << downTotalBytes << "\n        }";
        }

        out << "\n      ]\n"
            << "    }";
    }

    if (!firstWindow)
    {
        out << "\n";
    }

    const std::vector<MacSummaryStats> summaryFromSparse = BuildSparseMacSummary(statistics);

    bool summaryTotalsConsistent = summaryFromWindows.size() == summaryFromSparse.size();

    if (summaryTotalsConsistent)
    {
        for (std::size_t apId = 0; apId < summaryFromWindows.size(); ++apId)
        {
            if (!MacSummaryEqual(summaryFromWindows[apId], summaryFromSparse[apId]))
            {
                summaryTotalsConsistent = false;
                NS_LOG_ERROR("[MAC stats] summary mismatch for AP " << apId);
            }
        }
    }

    out << "  ],\n"
        << "  \"summary\": [\n";

    for (std::size_t apId = 0; apId < summaryFromWindows.size(); ++apId)
    {
        const auto& summary = summaryFromWindows[apId];

        out << "    {\n"
            << "      \"ap_id\": " << apId << ",\n"
            << "      \"up_total_bytes\": " << summary.upTotalBytes << ",\n"
            << "      \"down_total_bytes\": " << summary.downTotalBytes << ",\n"
            << "      \"up_flows\": ";

        WriteMacSummaryFlowArray(out,
                                 statistics.stationIpsByBss[apId],
                                 summary.upBytes,
                                 summary.upPhyRates,
                                 "        ");

        out << ",\n"
            << "      \"down_flows\": ";

        WriteMacSummaryFlowArray(out,
                                 statistics.stationIpsByBss[apId],
                                 summary.downBytes,
                                 summary.downPhyRates,
                                 "        ");

        out << "\n"
            << "    }";

        if (apId + 1 < summaryFromWindows.size())
        {
            out << ",";
        }
        out << "\n";
    }

    out << "  ],\n"
        << "  \"validation\": {\n"
        << "    \"window_totals_consistent\": " << (windowTotalsConsistent ? "true" : "false")
        << ",\n"
        << "    \"summary_totals_consistent\": " << (summaryTotalsConsistent ? "true" : "false")
        << "\n"
        << "  }\n"
        << "}\n";

    out.close();

    NS_LOG_INFO("PHY per-node statistics written to "
                << outputPath << " (" << emittedWindowCount << " non-empty / " << windowCount
                << " total x " << kMacStatsWindowMs << "ms windows)"
                << ", windowTotalsConsistent=" << (windowTotalsConsistent ? "true" : "false")
                << ", summaryTotalsConsistent=" << (summaryTotalsConsistent ? "true" : "false"));
}

void
WifiStatistics::WriteJson(const std::string& outputPath) const
{
    WriteWifiStatisticsJson(*m_state, outputPath);
}

} // namespace ns3
