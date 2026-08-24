#include "scenario-log.h"
#include "traffic-coordinator.h"
#include "wifi-statistics-internal.h"
#include "wifi-statistics.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <map>

namespace ns3
{

static LogComponent& g_log = llm_example::GetScenarioLog();

static void
PrintCrossLayerStats(const WifiStatisticsState& statistics)
{
    NS_LOG_WARN("========== App -> PHY / reliability statistics ==========");

    const uint32_t totalSecondBuckets =
        statistics.coordinator.GetMaxExperimentDurationMs() > 0.0
            ? static_cast<uint32_t>(
                  std::ceil(statistics.coordinator.GetMaxExperimentDurationMs() / 1000.0))
            : 0;
    const double experimentDurationSeconds =
        statistics.coordinator.GetMaxExperimentDurationMs() / 1000.0;

    // Iterate the topology registry, not only the sparse statistics map.  This
    // makes unused STAs visible too (all-zero rows are meaningful diagnostics).
    for (const auto& [nodeId, label] : statistics.nodeLabels)
    {
        DelayAccumulator overallDelay;
        uint64_t totalAppTxBytes = 0;
        uint64_t totalPhyPayload = 0;
        uint64_t totalUniquePayload = 0;
        uint64_t totalPhyMpduBytes = 0;
        uint64_t totalRetransmissions = 0;
        uint64_t totalMacDrops = 0;
        uint64_t totalMacDropBytes = 0;
        uint64_t totalMacMpduDrops = 0;
        uint64_t totalMacMpduDropBytes = 0;
        std::map<int, uint64_t> totalMacMpduDropsByReason;
        uint64_t totalMacFailures = 0;
        uint64_t totalMacFinalFailures = 0;
        uint64_t totalAppDrops = 0;
        uint64_t totalAppDropBytes = 0;
        int64_t totalBusyUs = 0;

        const auto nodeIt = statistics.nodeSeconds.find(nodeId);

        for (uint32_t second = 0; second < totalSecondBuckets; ++second)
        {
            static const NodeSecondStats emptyStats;
            const NodeSecondStats* stats = &emptyStats;
            if (nodeIt != statistics.nodeSeconds.end())
            {
                const auto secondIt = nodeIt->second.find(second);
                if (secondIt != nodeIt->second.end())
                {
                    stats = &secondIt->second;
                }
            }

            const double bucketStartSeconds = static_cast<double>(second);
            const double bucketDurationSeconds =
                std::max(0.0, std::min(1.0, experimentDurationSeconds - bucketStartSeconds));
            const double denominator = bucketDurationSeconds > 0.0 ? bucketDurationSeconds : 1.0;

            const double appThroughputMbps =
                static_cast<double>(stats->appTxBytes) * 8.0 / 1e6 / denominator;
            const double throughputMbps =
                static_cast<double>(stats->phyPayloadBytes) * 8.0 / 1e6 / denominator;
            const double uniqueThroughputMbps =
                static_cast<double>(stats->phyUniquePayloadBytes) * 8.0 / 1e6 / denominator;
            const double channelUtilization =
                bucketDurationSeconds > 0.0 ? std::min(100.0,
                                                       static_cast<double>(stats->phyBusyUs) /
                                                           (bucketDurationSeconds * 1e6) * 100.0)
                                            : 0.0;

            NS_LOG_WARN(
                "[Node stats] node="
                << label << " second=" << second << " app_to_phy_count=" << stats->appToPhy.count
                << " app_to_phy_mean_us=" << stats->appToPhy.MeanUs()
                << " app_to_phy_stddev_us=" << stats->appToPhy.StdDevUs()
                << " app_to_phy_min_us=" << (stats->appToPhy.count ? stats->appToPhy.minUs : 0.0)
                << " app_to_phy_max_us=" << stats->appToPhy.maxUs
                << " app_tx_mbps=" << appThroughputMbps << " phy_payload_mbps=" << throughputMbps
                << " phy_unique_payload_mbps=" << uniqueThroughputMbps
                << " channel_utilization=" << channelUtilization << "%"
                << " phy_retrans=" << stats->phyRetransmissions << " mac_tx_drops="
                << stats->macTxDrops << " mac_tx_drop_bytes=" << stats->macTxDropBytes
                << " mac_mpdu_drops=" << stats->macMpduDrops << " mac_mpdu_drop_bytes="
                << stats->macMpduDropBytes << " mac_data_failed=" << stats->macDataFailures
                << " mac_final_data_failed=" << stats->macFinalDataFailures << " app_drop_events="
                << stats->appDropEvents << " app_drop_bytes=" << stats->appDropBytes);

            for (const auto& [reason, count] : stats->macMpduDropsByReason)
            {
                NS_LOG_WARN("[MAC MPDU drop] node=" << label << " second=" << second
                                                    << " reason=" << reason << " count=" << count);
            }

            for (const auto& [agentKey, drop] : stats->appDropsByAgent)
            {
                NS_LOG_WARN("[App drop] node="
                            << label << " second=" << second << " agent=\"" << agentKey << "\""
                            << " events=" << drop.events << " bytes=" << drop.bytes);
            }

            overallDelay.Merge(stats->appToPhy);
            totalAppTxBytes += stats->appTxBytes;
            totalPhyPayload += stats->phyPayloadBytes;
            totalUniquePayload += stats->phyUniquePayloadBytes;
            totalPhyMpduBytes += stats->phyMpduBytes;
            totalRetransmissions += stats->phyRetransmissions;
            totalMacDrops += stats->macTxDrops;
            totalMacDropBytes += stats->macTxDropBytes;
            totalMacMpduDrops += stats->macMpduDrops;
            totalMacMpduDropBytes += stats->macMpduDropBytes;
            for (const auto& [reason, count] : stats->macMpduDropsByReason)
            {
                totalMacMpduDropsByReason[reason] += count;
            }
            totalMacFailures += stats->macDataFailures;
            totalMacFinalFailures += stats->macFinalDataFailures;
            totalAppDrops += stats->appDropEvents;
            totalAppDropBytes += stats->appDropBytes;
            totalBusyUs += stats->phyBusyUs;
        }

        NS_LOG_WARN(
            "[Node overall] node="
            << label << " seconds=" << experimentDurationSeconds << " app_to_phy_count="
            << overallDelay.count << " app_to_phy_mean_us=" << overallDelay.MeanUs()
            << " app_to_phy_stddev_us=" << overallDelay.StdDevUs()
            << " app_to_phy_min_us=" << (overallDelay.count ? overallDelay.minUs : 0.0)
            << " app_to_phy_max_us=" << overallDelay.maxUs << " app_tx_bytes=" << totalAppTxBytes
            << " phy_payload_bytes=" << totalPhyPayload << " phy_unique_payload_bytes="
            << totalUniquePayload << " phy_mpdu_bytes=" << totalPhyMpduBytes << " avg_app_tx_mbps="
            << (experimentDurationSeconds > 0.0
                    ? static_cast<double>(totalAppTxBytes) * 8.0 / 1e6 / experimentDurationSeconds
                    : 0.0)
            << " avg_phy_payload_mbps="
            << (experimentDurationSeconds > 0.0
                    ? static_cast<double>(totalPhyPayload) * 8.0 / 1e6 / experimentDurationSeconds
                    : 0.0)
            << " avg_channel_utilization="
            << (experimentDurationSeconds > 0.0
                    ? std::min(100.0,
                               static_cast<double>(totalBusyUs) /
                                   (experimentDurationSeconds * 1e6) * 100.0)
                    : 0.0)
            << "% phy_retrans=" << totalRetransmissions << " mac_tx_drops=" << totalMacDrops
            << " mac_tx_drop_bytes=" << totalMacDropBytes << " mac_mpdu_drops=" << totalMacMpduDrops
            << " mac_mpdu_drop_bytes=" << totalMacMpduDropBytes << " mac_data_failed="
            << totalMacFailures << " mac_final_data_failed=" << totalMacFinalFailures
            << " app_drop_events=" << totalAppDrops << " app_drop_bytes=" << totalAppDropBytes);

        for (const auto& [reason, count] : totalMacMpduDropsByReason)
        {
            NS_LOG_WARN("[MAC MPDU drop overall] node=" << label << " reason=" << reason
                                                        << " count=" << count);
        }
    }

    NS_LOG_WARN("==========================================================");
}

void
WifiStatistics::PrintCrossLayerReport() const
{
    PrintCrossLayerStats(*m_state);
}

} // namespace ns3
