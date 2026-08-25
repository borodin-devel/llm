#ifndef EXPERIMENT_OUTPUT_H
#define EXPERIMENT_OUTPUT_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ns3
{

/**
 * Aggregate transmission measurements for one sender.
 *
 * MAC transmit payload samples may include repeated transmission attempts.
 * The effective throughput is absent when no matched transmit/receive pair
 * has a positive duration.
 */
struct TransmissionSenderSummary
{
    std::string senderIpv4;                  ///< Sender IPv4 address.
    uint64_t matchedPacketCount{0};          ///< Positive-duration matched packet count.
    uint64_t totalTransmissionDurationUs{0}; ///< Sum of matched transmission durations in us.
    uint64_t transmittedPayloadBytes{0};     ///< MAC transmit payload bytes, including repeats.
    std::optional<double> effectiveThroughputMbps; ///< Effective throughput in Mbps, if measurable.
};

/** Aggregate transmission measurements for all senders. */
struct TransmissionSummary
{
    std::vector<TransmissionSenderSummary>
        senders; ///< Measurements ordered by sender IPv4 address.
};

/** Application-to-PHY delay distribution in microseconds. */
struct DelaySummary
{
    uint64_t sampleCount{0};         ///< Number of delay samples.
    double meanUs{0.0};              ///< Mean delay in microseconds.
    double standardDeviationUs{0.0}; ///< Population standard deviation in microseconds.
    double minimumUs{0.0};           ///< Minimum delay in microseconds.
    double maximumUs{0.0};           ///< Maximum delay in microseconds.
};

/** MAC MPDU drops attributed to one reason code. */
struct MacDropReasonSummary
{
    int reasonCode{0};     ///< Numeric Wi-Fi MAC drop reason.
    uint64_t dropCount{0}; ///< Number of MPDU drops for the reason.
};

/** Application drops attributed to one agent. */
struct AgentDropSummary
{
    std::string agentKey;            ///< Stable application agent key.
    uint64_t dropEventCount{0};      ///< Number of application drop events.
    uint64_t droppedPayloadBytes{0}; ///< Dropped application payload bytes.
};

/** Cross-layer measurements for one node and one configured one-second interval. */
struct CrossLayerIntervalSummary
{
    uint64_t intervalIndex{0};                              ///< Zero-based interval index.
    double intervalStartS{0.0};                             ///< Interval start in seconds.
    double intervalDurationS{0.0};                          ///< Interval duration in seconds.
    DelaySummary applicationToPhyDelay;                     ///< Application-to-PHY delay summary.
    double applicationTransmitThroughputMbps{0.0};          ///< Application throughput in Mbps.
    double phyPayloadThroughputMbps{0.0};                   ///< PHY payload throughput in Mbps.
    double uniquePhyPayloadThroughputMbps{0.0};             ///< Unique PHY throughput in Mbps.
    double channelUtilizationPercent{0.0};                  ///< PHY busy-time percentage.
    uint64_t phyRetransmissionCount{0};                     ///< PHY retransmission count.
    uint64_t macTransmitDropCount{0};                       ///< MAC transmit-drop count.
    uint64_t macTransmitDropBytes{0};                       ///< MAC transmit-drop bytes.
    uint64_t macMpduDropCount{0};                           ///< MAC MPDU-drop count.
    uint64_t macMpduDropBytes{0};                           ///< MAC MPDU-drop bytes.
    uint64_t macDataFailureCount{0};                        ///< MAC data-failure count.
    uint64_t macFinalDataFailureCount{0};                   ///< MAC final data-failure count.
    uint64_t applicationDropEventCount{0};                  ///< Application drop-event count.
    uint64_t applicationDropBytes{0};                       ///< Application drop bytes.
    std::vector<MacDropReasonSummary> macMpduDropsByReason; ///< MPDU drops ordered by reason.
    std::vector<AgentDropSummary> applicationDropsByAgent;  ///< Drops ordered by agent key.
};

/** Aggregate cross-layer measurements for one node over the experiment. */
struct CrossLayerOverallSummary
{
    double experimentDurationS{0.0};                      ///< Experiment duration in seconds.
    DelaySummary applicationToPhyDelay;                   ///< Application-to-PHY delay summary.
    uint64_t applicationTransmittedPayloadBytes{0};       ///< Application transmit payload bytes.
    uint64_t phyPayloadBytes{0};                          ///< PHY payload bytes including retries.
    uint64_t uniquePhyPayloadBytes{0};                    ///< Deduplicated PHY payload bytes.
    uint64_t phyMpduBytes{0};                             ///< Complete tagged PHY MPDU bytes.
    double averageApplicationTransmitThroughputMbps{0.0}; ///< Average application throughput.
    double averagePhyPayloadThroughputMbps{0.0};          ///< Average PHY payload throughput.
    double averageChannelUtilizationPercent{0.0};         ///< Average PHY busy-time percentage.
    uint64_t phyRetransmissionCount{0};                   ///< PHY retransmission count.
    uint64_t macTransmitDropCount{0};                     ///< MAC transmit-drop count.
    uint64_t macTransmitDropBytes{0};                     ///< MAC transmit-drop bytes.
    uint64_t macMpduDropCount{0};                         ///< MAC MPDU-drop count.
    uint64_t macMpduDropBytes{0};                         ///< MAC MPDU-drop bytes.
    uint64_t macDataFailureCount{0};                      ///< MAC data-failure count.
    uint64_t macFinalDataFailureCount{0};                 ///< MAC final data-failure count.
    uint64_t applicationDropEventCount{0};                ///< Application drop-event count.
    uint64_t applicationDropBytes{0};                     ///< Application drop bytes.
    std::vector<MacDropReasonSummary> macMpduDropsByReason; ///< MPDU drops ordered by reason.
};

/** Cross-layer interval and overall measurements for one registered node. */
struct CrossLayerNodeSummary
{
    uint32_t nodeId{0};                                        ///< ns-3 node identifier.
    std::string nodeLabel;                                     ///< Stable report label.
    std::vector<CrossLayerIntervalSummary> oneSecondIntervals; ///< Configured intervals.
    CrossLayerOverallSummary overall;                          ///< Whole-experiment measurements.
};

/** Cross-layer measurements for every registered node. */
struct CrossLayerSummary
{
    std::vector<CrossLayerNodeSummary> nodes; ///< Measurements ordered by node identifier.
};

} // namespace ns3

#endif // EXPERIMENT_OUTPUT_H
