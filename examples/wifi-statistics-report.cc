#include "scenario-log.h"
#include "traffic-coordinator.h"
#include "wifi-statistics-internal.h"
#include "wifi-statistics.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <utility>

namespace ns3
{

static LogComponent& g_log = llm_example::GetScenarioLog();

static DelaySummary
BuildDelaySummary(const DelayAccumulator& accumulator)
{
    return {
        .sampleCount = accumulator.count,
        .meanUs = accumulator.MeanUs(),
        .standardDeviationUs = accumulator.StdDevUs(),
        .minimumUs = accumulator.count > 0 ? accumulator.minUs : 0.0,
        .maximumUs = accumulator.count > 0 ? accumulator.maxUs : 0.0,
    };
}

static std::vector<MacDropReasonSummary>
BuildMacDropReasonSummaries(const std::map<int, uint64_t>& dropsByReason)
{
    std::vector<MacDropReasonSummary> summaries;
    summaries.reserve(dropsByReason.size());
    for (const auto& [reasonCode, dropCount] : dropsByReason)
    {
        summaries.push_back({reasonCode, dropCount});
    }
    return summaries;
}

static std::vector<AgentDropSummary>
BuildAgentDropSummaries(const std::map<std::string, AgentDropStats>& dropsByAgent)
{
    std::vector<AgentDropSummary> summaries;
    summaries.reserve(dropsByAgent.size());
    for (const auto& [agentKey, drop] : dropsByAgent)
    {
        summaries.push_back({agentKey, drop.events, drop.bytes});
    }
    return summaries;
}

static CrossLayerIntervalSummary
BuildIntervalSummary(uint64_t intervalIndex,
                     double intervalDurationS,
                     const NodeSecondStats& statistics)
{
    const double denominator = intervalDurationS > 0.0 ? intervalDurationS : 1.0;
    return {
        .intervalIndex = intervalIndex,
        .intervalStartS = static_cast<double>(intervalIndex),
        .intervalDurationS = intervalDurationS,
        .applicationToPhyDelay = BuildDelaySummary(statistics.appToPhy),
        .applicationTransmitThroughputMbps =
            static_cast<double>(statistics.appTxBytes) * 8.0 / 1e6 / denominator,
        .phyPayloadThroughputMbps =
            static_cast<double>(statistics.phyPayloadBytes) * 8.0 / 1e6 / denominator,
        .uniquePhyPayloadThroughputMbps =
            static_cast<double>(statistics.phyUniquePayloadBytes) * 8.0 / 1e6 / denominator,
        .channelUtilizationPercent = intervalDurationS > 0.0
                                         ? std::min(100.0,
                                                    static_cast<double>(statistics.phyBusyUs) /
                                                        (intervalDurationS * 1e6) * 100.0)
                                         : 0.0,
        .phyRetransmissionCount = statistics.phyRetransmissions,
        .macTransmitDropCount = statistics.macTxDrops,
        .macTransmitDropBytes = statistics.macTxDropBytes,
        .macMpduDropCount = statistics.macMpduDrops,
        .macMpduDropBytes = statistics.macMpduDropBytes,
        .macDataFailureCount = statistics.macDataFailures,
        .macFinalDataFailureCount = statistics.macFinalDataFailures,
        .applicationDropEventCount = statistics.appDropEvents,
        .applicationDropBytes = statistics.appDropBytes,
        .macMpduDropsByReason = BuildMacDropReasonSummaries(statistics.macMpduDropsByReason),
        .applicationDropsByAgent = BuildAgentDropSummaries(statistics.appDropsByAgent),
    };
}

CrossLayerSummary
BuildCrossLayerSummary(const WifiStatisticsState& statistics)
{
    const double durationMs = statistics.coordinator.GetMaxExperimentDurationMs();
    const double durationS = durationMs / 1000.0;
    const uint64_t totalSecondBuckets =
        durationMs > 0.0 ? static_cast<uint64_t>(std::ceil(durationMs / 1000.0)) : 0;

    CrossLayerSummary summary;
    summary.nodes.reserve(statistics.nodeLabels.size());
    for (const auto& [nodeId, nodeLabel] : statistics.nodeLabels)
    {
        CrossLayerNodeSummary nodeSummary;
        nodeSummary.nodeId = nodeId;
        nodeSummary.nodeLabel = nodeLabel;
        nodeSummary.oneSecondIntervals.reserve(totalSecondBuckets);

        DelayAccumulator overallDelay;
        uint64_t totalAppTxBytes = 0;
        uint64_t totalPhyPayloadBytes = 0;
        uint64_t totalUniquePhyPayloadBytes = 0;
        uint64_t totalPhyMpduBytes = 0;
        uint64_t totalPhyRetransmissions = 0;
        uint64_t totalMacTransmitDrops = 0;
        uint64_t totalMacTransmitDropBytes = 0;
        uint64_t totalMacMpduDrops = 0;
        uint64_t totalMacMpduDropBytes = 0;
        uint64_t totalMacDataFailures = 0;
        uint64_t totalMacFinalDataFailures = 0;
        uint64_t totalApplicationDrops = 0;
        uint64_t totalApplicationDropBytes = 0;
        int64_t totalBusyUs = 0;
        std::map<int, uint64_t> totalMacMpduDropsByReason;

        const auto nodeIt = statistics.nodeSeconds.find(nodeId);
        for (uint64_t intervalIndex = 0; intervalIndex < totalSecondBuckets; ++intervalIndex)
        {
            static const NodeSecondStats emptyStatistics;
            const NodeSecondStats* intervalStatistics = &emptyStatistics;
            if (nodeIt != statistics.nodeSeconds.end())
            {
                const auto intervalIt = nodeIt->second.find(intervalIndex);
                if (intervalIt != nodeIt->second.end())
                {
                    intervalStatistics = &intervalIt->second;
                }
            }

            const double intervalDurationS =
                std::max(0.0, std::min(1.0, durationS - static_cast<double>(intervalIndex)));
            nodeSummary.oneSecondIntervals.push_back(
                BuildIntervalSummary(intervalIndex, intervalDurationS, *intervalStatistics));

            overallDelay.Merge(intervalStatistics->appToPhy);
            totalAppTxBytes += intervalStatistics->appTxBytes;
            totalPhyPayloadBytes += intervalStatistics->phyPayloadBytes;
            totalUniquePhyPayloadBytes += intervalStatistics->phyUniquePayloadBytes;
            totalPhyMpduBytes += intervalStatistics->phyMpduBytes;
            totalPhyRetransmissions += intervalStatistics->phyRetransmissions;
            totalMacTransmitDrops += intervalStatistics->macTxDrops;
            totalMacTransmitDropBytes += intervalStatistics->macTxDropBytes;
            totalMacMpduDrops += intervalStatistics->macMpduDrops;
            totalMacMpduDropBytes += intervalStatistics->macMpduDropBytes;
            totalMacDataFailures += intervalStatistics->macDataFailures;
            totalMacFinalDataFailures += intervalStatistics->macFinalDataFailures;
            totalApplicationDrops += intervalStatistics->appDropEvents;
            totalApplicationDropBytes += intervalStatistics->appDropBytes;
            totalBusyUs += intervalStatistics->phyBusyUs;
            for (const auto& [reasonCode, dropCount] : intervalStatistics->macMpduDropsByReason)
            {
                totalMacMpduDropsByReason[reasonCode] += dropCount;
            }
        }

        nodeSummary.overall = {
            .experimentDurationS = durationS,
            .applicationToPhyDelay = BuildDelaySummary(overallDelay),
            .applicationTransmittedPayloadBytes = totalAppTxBytes,
            .phyPayloadBytes = totalPhyPayloadBytes,
            .uniquePhyPayloadBytes = totalUniquePhyPayloadBytes,
            .phyMpduBytes = totalPhyMpduBytes,
            .averageApplicationTransmitThroughputMbps =
                durationS > 0.0 ? static_cast<double>(totalAppTxBytes) * 8.0 / 1e6 / durationS
                                : 0.0,
            .averagePhyPayloadThroughputMbps =
                durationS > 0.0 ? static_cast<double>(totalPhyPayloadBytes) * 8.0 / 1e6 / durationS
                                : 0.0,
            .averageChannelUtilizationPercent =
                durationS > 0.0
                    ? std::min(100.0, static_cast<double>(totalBusyUs) / (durationS * 1e6) * 100.0)
                    : 0.0,
            .phyRetransmissionCount = totalPhyRetransmissions,
            .macTransmitDropCount = totalMacTransmitDrops,
            .macTransmitDropBytes = totalMacTransmitDropBytes,
            .macMpduDropCount = totalMacMpduDrops,
            .macMpduDropBytes = totalMacMpduDropBytes,
            .macDataFailureCount = totalMacDataFailures,
            .macFinalDataFailureCount = totalMacFinalDataFailures,
            .applicationDropEventCount = totalApplicationDrops,
            .applicationDropBytes = totalApplicationDropBytes,
            .macMpduDropsByReason = BuildMacDropReasonSummaries(totalMacMpduDropsByReason),
        };
        summary.nodes.push_back(std::move(nodeSummary));
    }
    return summary;
}

static void
PrintCrossLayerStats(const CrossLayerSummary& summary)
{
    NS_LOG_WARN("========== App -> PHY / reliability statistics ==========");
    for (const auto& node : summary.nodes)
    {
        for (const auto& interval : node.oneSecondIntervals)
        {
            NS_LOG_WARN(
                "[Node stats] node="
                << node.nodeLabel << " second=" << interval.intervalIndex
                << " app_to_phy_count=" << interval.applicationToPhyDelay.sampleCount
                << " app_to_phy_mean_us=" << interval.applicationToPhyDelay.meanUs
                << " app_to_phy_stddev_us=" << interval.applicationToPhyDelay.standardDeviationUs
                << " app_to_phy_min_us=" << interval.applicationToPhyDelay.minimumUs
                << " app_to_phy_max_us=" << interval.applicationToPhyDelay.maximumUs
                << " app_tx_mbps=" << interval.applicationTransmitThroughputMbps
                << " phy_payload_mbps=" << interval.phyPayloadThroughputMbps
                << " phy_unique_payload_mbps=" << interval.uniquePhyPayloadThroughputMbps
                << " channel_utilization=" << interval.channelUtilizationPercent
                << "% phy_retrans=" << interval.phyRetransmissionCount
                << " mac_tx_drops=" << interval.macTransmitDropCount << " mac_tx_drop_bytes="
                << interval.macTransmitDropBytes << " mac_mpdu_drops=" << interval.macMpduDropCount
                << " mac_mpdu_drop_bytes=" << interval.macMpduDropBytes
                << " mac_data_failed=" << interval.macDataFailureCount
                << " mac_final_data_failed=" << interval.macFinalDataFailureCount
                << " app_drop_events=" << interval.applicationDropEventCount
                << " app_drop_bytes=" << interval.applicationDropBytes);

            for (const auto& reason : interval.macMpduDropsByReason)
            {
                NS_LOG_WARN("[MAC MPDU drop] node="
                            << node.nodeLabel << " second=" << interval.intervalIndex
                            << " reason=" << reason.reasonCode << " count=" << reason.dropCount);
            }
            for (const auto& agent : interval.applicationDropsByAgent)
            {
                NS_LOG_WARN("[App drop] node=" << node.nodeLabel
                                               << " second=" << interval.intervalIndex
                                               << " agent=\"" << agent.agentKey << "\""
                                               << " events=" << agent.dropEventCount
                                               << " bytes=" << agent.droppedPayloadBytes);
            }
        }

        const auto& overall = node.overall;
        NS_LOG_WARN("[Node overall] node="
                    << node.nodeLabel << " seconds=" << overall.experimentDurationS
                    << " app_to_phy_count=" << overall.applicationToPhyDelay.sampleCount
                    << " app_to_phy_mean_us=" << overall.applicationToPhyDelay.meanUs
                    << " app_to_phy_stddev_us=" << overall.applicationToPhyDelay.standardDeviationUs
                    << " app_to_phy_min_us=" << overall.applicationToPhyDelay.minimumUs
                    << " app_to_phy_max_us=" << overall.applicationToPhyDelay.maximumUs
                    << " app_tx_bytes=" << overall.applicationTransmittedPayloadBytes
                    << " phy_payload_bytes=" << overall.phyPayloadBytes
                    << " phy_unique_payload_bytes=" << overall.uniquePhyPayloadBytes
                    << " phy_mpdu_bytes=" << overall.phyMpduBytes
                    << " avg_app_tx_mbps=" << overall.averageApplicationTransmitThroughputMbps
                    << " avg_phy_payload_mbps=" << overall.averagePhyPayloadThroughputMbps
                    << " avg_channel_utilization=" << overall.averageChannelUtilizationPercent
                    << "% phy_retrans=" << overall.phyRetransmissionCount
                    << " mac_tx_drops=" << overall.macTransmitDropCount
                    << " mac_tx_drop_bytes=" << overall.macTransmitDropBytes
                    << " mac_mpdu_drops=" << overall.macMpduDropCount
                    << " mac_mpdu_drop_bytes=" << overall.macMpduDropBytes
                    << " mac_data_failed=" << overall.macDataFailureCount
                    << " mac_final_data_failed=" << overall.macFinalDataFailureCount
                    << " app_drop_events=" << overall.applicationDropEventCount
                    << " app_drop_bytes=" << overall.applicationDropBytes);

        for (const auto& reason : overall.macMpduDropsByReason)
        {
            NS_LOG_WARN("[MAC MPDU drop overall] node=" << node.nodeLabel
                                                        << " reason=" << reason.reasonCode
                                                        << " count=" << reason.dropCount);
        }
    }
    NS_LOG_WARN("==========================================================");
}

CrossLayerSummary
WifiStatistics::BuildCrossLayerSummary() const
{
    return ns3::BuildCrossLayerSummary(*m_state);
}

void
WifiStatistics::PrintCrossLayerReport() const
{
    PrintCrossLayerStats(BuildCrossLayerSummary());
}

} // namespace ns3
