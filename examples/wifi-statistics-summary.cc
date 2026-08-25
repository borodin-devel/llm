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
    const int64_t durationUs =
        ConvertExperimentDurationMsToUs(statistics.coordinator.GetMaxExperimentDurationMs());
    const double durationS = static_cast<double>(durationUs) / 1e6;
    const uint64_t totalSecondBuckets =
        durationUs > 0 ? static_cast<uint64_t>(durationUs / 1000000 + (durationUs % 1000000 != 0))
                       : 0;

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

        const auto nodeIterator = statistics.nodeSeconds.find(nodeId);
        for (uint64_t intervalIndex = 0; intervalIndex < totalSecondBuckets; ++intervalIndex)
        {
            static const NodeSecondStats emptyStatistics;
            const NodeSecondStats* intervalStatistics = &emptyStatistics;
            if (nodeIterator != statistics.nodeSeconds.end())
            {
                const auto intervalIterator = nodeIterator->second.find(intervalIndex);
                if (intervalIterator != nodeIterator->second.end())
                {
                    intervalStatistics = &intervalIterator->second;
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

CrossLayerSummary
WifiStatistics::BuildCrossLayerSummary() const
{
    return ns3::BuildCrossLayerSummary(*m_state);
}

} // namespace ns3
