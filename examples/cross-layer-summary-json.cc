#include "experiment-output-internal.h"

#include <ostream>

namespace ns3
{

static void
WriteDelaySummaryJson(std::ostream& output, const DelaySummary& delay)
{
    output << "{\"sample_count\": ";
    WriteJsonScalar(output, delay.sampleCount);
    output << ", \"mean_us\": ";
    WriteJsonScalar(output, delay.meanUs);
    output << ", \"standard_deviation_us\": ";
    WriteJsonScalar(output, delay.standardDeviationUs);
    output << ", \"minimum_us\": ";
    WriteJsonScalar(output, delay.minimumUs);
    output << ", \"maximum_us\": ";
    WriteJsonScalar(output, delay.maximumUs);
    output << '}';
}

static void
WriteMacDropReasonsJson(std::ostream& output, const std::vector<MacDropReasonSummary>& dropReasons)
{
    output << '[';
    bool first = true;
    for (const auto& reason : dropReasons)
    {
        output << (first ? "" : ",") << "{\"reason_code\": ";
        WriteJsonScalar(output, reason.reasonCode);
        output << ", \"drop_count\": ";
        WriteJsonScalar(output, reason.dropCount);
        output << '}';
        first = false;
    }
    output << ']';
}

static void
WriteAgentDropsJson(std::ostream& output, const std::vector<AgentDropSummary>& agentDrops)
{
    output << '[';
    bool first = true;
    for (const auto& agent : agentDrops)
    {
        output << (first ? "" : ",") << "{\"agent_key\": ";
        WriteJsonScalar(output, agent.agentKey);
        output << ", \"drop_event_count\": ";
        WriteJsonScalar(output, agent.dropEventCount);
        output << ", \"dropped_payload_bytes\": ";
        WriteJsonScalar(output, agent.droppedPayloadBytes);
        output << '}';
        first = false;
    }
    output << ']';
}

static void
WriteCrossLayerIntervalJson(std::ostream& output, const CrossLayerIntervalSummary& interval)
{
    output << "{\"interval_index\": ";
    WriteJsonScalar(output, interval.intervalIndex);
    output << ", \"interval_start_s\": ";
    WriteJsonScalar(output, interval.intervalStartS);
    output << ", \"interval_duration_s\": ";
    WriteJsonScalar(output, interval.intervalDurationS);
    output << ", \"application_to_phy_delay\": ";
    WriteDelaySummaryJson(output, interval.applicationToPhyDelay);
    output << ", \"application_transmit_throughput_mbps\": ";
    WriteJsonScalar(output, interval.applicationTransmitThroughputMbps);
    output << ", \"phy_payload_throughput_mbps\": ";
    WriteJsonScalar(output, interval.phyPayloadThroughputMbps);
    output << ", \"unique_phy_payload_throughput_mbps\": ";
    WriteJsonScalar(output, interval.uniquePhyPayloadThroughputMbps);
    output << ", \"channel_utilization_percent\": ";
    WriteJsonScalar(output, interval.channelUtilizationPercent);
    output << ", \"phy_retransmission_count\": ";
    WriteJsonScalar(output, interval.phyRetransmissionCount);
    output << ", \"mac_transmit_drop_count\": ";
    WriteJsonScalar(output, interval.macTransmitDropCount);
    output << ", \"mac_transmit_drop_bytes\": ";
    WriteJsonScalar(output, interval.macTransmitDropBytes);
    output << ", \"mac_mpdu_drop_count\": ";
    WriteJsonScalar(output, interval.macMpduDropCount);
    output << ", \"mac_mpdu_drop_bytes\": ";
    WriteJsonScalar(output, interval.macMpduDropBytes);
    output << ", \"mac_data_failure_count\": ";
    WriteJsonScalar(output, interval.macDataFailureCount);
    output << ", \"mac_final_data_failure_count\": ";
    WriteJsonScalar(output, interval.macFinalDataFailureCount);
    output << ", \"application_drop_event_count\": ";
    WriteJsonScalar(output, interval.applicationDropEventCount);
    output << ", \"application_drop_bytes\": ";
    WriteJsonScalar(output, interval.applicationDropBytes);
    output << ", \"mac_mpdu_drops_by_reason\": ";
    WriteMacDropReasonsJson(output, interval.macMpduDropsByReason);
    output << ", \"application_drops_by_agent\": ";
    WriteAgentDropsJson(output, interval.applicationDropsByAgent);
    output << '}';
}

static void
WriteCrossLayerOverallJson(std::ostream& output, const CrossLayerOverallSummary& overall)
{
    output << "{\"experiment_duration_s\": ";
    WriteJsonScalar(output, overall.experimentDurationS);
    output << ", \"application_to_phy_delay\": ";
    WriteDelaySummaryJson(output, overall.applicationToPhyDelay);
    output << ", \"application_transmitted_payload_bytes\": ";
    WriteJsonScalar(output, overall.applicationTransmittedPayloadBytes);
    output << ", \"phy_payload_bytes\": ";
    WriteJsonScalar(output, overall.phyPayloadBytes);
    output << ", \"unique_phy_payload_bytes\": ";
    WriteJsonScalar(output, overall.uniquePhyPayloadBytes);
    output << ", \"phy_mpdu_bytes\": ";
    WriteJsonScalar(output, overall.phyMpduBytes);
    output << ", \"average_application_transmit_throughput_mbps\": ";
    WriteJsonScalar(output, overall.averageApplicationTransmitThroughputMbps);
    output << ", \"average_phy_payload_throughput_mbps\": ";
    WriteJsonScalar(output, overall.averagePhyPayloadThroughputMbps);
    output << ", \"average_channel_utilization_percent\": ";
    WriteJsonScalar(output, overall.averageChannelUtilizationPercent);
    output << ", \"phy_retransmission_count\": ";
    WriteJsonScalar(output, overall.phyRetransmissionCount);
    output << ", \"mac_transmit_drop_count\": ";
    WriteJsonScalar(output, overall.macTransmitDropCount);
    output << ", \"mac_transmit_drop_bytes\": ";
    WriteJsonScalar(output, overall.macTransmitDropBytes);
    output << ", \"mac_mpdu_drop_count\": ";
    WriteJsonScalar(output, overall.macMpduDropCount);
    output << ", \"mac_mpdu_drop_bytes\": ";
    WriteJsonScalar(output, overall.macMpduDropBytes);
    output << ", \"mac_data_failure_count\": ";
    WriteJsonScalar(output, overall.macDataFailureCount);
    output << ", \"mac_final_data_failure_count\": ";
    WriteJsonScalar(output, overall.macFinalDataFailureCount);
    output << ", \"application_drop_event_count\": ";
    WriteJsonScalar(output, overall.applicationDropEventCount);
    output << ", \"application_drop_bytes\": ";
    WriteJsonScalar(output, overall.applicationDropBytes);
    output << ", \"mac_mpdu_drops_by_reason\": ";
    WriteMacDropReasonsJson(output, overall.macMpduDropsByReason);
    output << '}';
}

void
WriteCrossLayerSummaryJson(std::ostream& output, const CrossLayerSummary& summary)
{
    output << "{\"nodes\": [";
    bool firstNode = true;
    for (const auto& node : summary.nodes)
    {
        output << (firstNode ? "\n" : ",\n") << "      {\"node_id\": ";
        WriteJsonScalar(output, node.nodeId);
        output << ", \"node_label\": ";
        WriteJsonScalar(output, node.nodeLabel);
        output << ", \"one_second_intervals\": [";
        bool firstInterval = true;
        for (const auto& interval : node.oneSecondIntervals)
        {
            output << (firstInterval ? "" : ",");
            WriteCrossLayerIntervalJson(output, interval);
            firstInterval = false;
        }
        output << "], \"overall\": ";
        WriteCrossLayerOverallJson(output, node.overall);
        output << '}';
        firstNode = false;
    }
    output << (firstNode ? "" : "\n    ") << "]}";
}

} // namespace ns3
