#ifndef EXPERIMENT_WINDOW_OUTPUT_H
#define EXPERIMENT_WINDOW_OUTPUT_H

#include "types.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ns3
{

/** Derived distribution for raw microsecond samples. */
struct SampleDistributionOutput
{
    uint64_t sampleCount{0};                   ///< Number of samples.
    std::optional<double> averageUs;           ///< Average in microseconds.
    std::optional<double> standardDeviationUs; ///< Population deviation in microseconds.
    std::optional<double> minimumUs;           ///< Minimum in microseconds.
    std::optional<double> maximumUs;           ///< Maximum in microseconds.
};

/** General device and cross-layer statistics for one direction. */
struct GeneralDirectionOutput
{
    uint64_t estimatedTransmittedTcpPayloadBytes{0}; ///< Estimated transmitted TCP payload bytes.
    uint64_t estimatedMatchedTcpPayloadBytes{0}; ///< Estimated positively matched payload bytes.
    uint64_t matchedPacketCount{0};              ///< Positive-duration matched packets.
    uint64_t totalTransmissionDurationUs{0};     ///< Total matched duration in microseconds.
    std::optional<double> averageTransmissionDurationUs; ///< Average matched duration.
    std::optional<double>
        transmissionDurationStandardDeviationUs; ///< Population deviation of matched durations.
    std::optional<double> minimumTransmissionDurationUs; ///< Minimum matched duration.
    std::optional<double> maximumTransmissionDurationUs; ///< Maximum matched duration.
    std::optional<double> effectiveThroughputMbps;       ///< Matched effective throughput.
    SampleDistributionOutput applicationToPhyDelay;      ///< Application-to-PHY delay distribution.
};

/** Application statistics for one agent. */
struct AppAgentOutput
{
    std::string agentKey;                                ///< Stable application agent key.
    uint64_t acceptedSendCount{0};                       ///< Accepted send-event count.
    uint64_t acceptedPayloadBytes{0};                    ///< Accepted payload bytes.
    std::optional<double> acceptedThroughputMbps;        ///< Accepted payload throughput.
    std::optional<double> acceptedBandwidthSharePercent; ///< Share of direction accepted bytes.
    uint64_t dropEventCount{0};                          ///< Application drop-event count.
    uint64_t droppedPayloadBytes{0};                     ///< Dropped payload bytes.
};

/** Application statistics for one peer. */
struct AppPeerOutput
{
    uint32_t peerNodeId{0};                              ///< Peer node identifier.
    std::string peerIpv4;                                ///< Peer IPv4 address.
    uint64_t acceptedSendCount{0};                       ///< Accepted send-event count.
    uint64_t acceptedPayloadBytes{0};                    ///< Accepted payload bytes.
    std::optional<double> acceptedThroughputMbps;        ///< Accepted payload throughput.
    std::optional<double> acceptedBandwidthSharePercent; ///< Share of accepted bytes.
    uint64_t receiveEventCount{0};                       ///< Receive-event count.
    uint64_t receivedPayloadBytes{0};                    ///< Received payload bytes.
    std::optional<double> receivedThroughputMbps;        ///< Received payload throughput.
    std::optional<double> receivedBandwidthSharePercent; ///< Share of received bytes.
    uint64_t dropEventCount{0};                          ///< Application drop-event count.
    uint64_t droppedPayloadBytes{0};                     ///< Dropped payload bytes.
};

/** Application statistics for one direction. */
struct AppDirectionOutput
{
    uint64_t acceptedSendCount{0};                    ///< Accepted send-event count.
    uint64_t acceptedPayloadBytes{0};                 ///< Accepted payload bytes.
    std::optional<double> acceptedThroughputMbps;     ///< Accepted payload throughput.
    uint64_t receiveEventCount{0};                    ///< Receive-event count.
    uint64_t receivedPayloadBytes{0};                 ///< Received payload bytes.
    std::optional<double> receivedThroughputMbps;     ///< Received payload throughput.
    uint64_t dropEventCount{0};                       ///< Application drop-event count.
    uint64_t droppedPayloadBytes{0};                  ///< Dropped payload bytes.
    SampleDistributionOutput receiveInterArrivalTime; ///< Receive inter-arrival distribution.
    std::vector<AppAgentOutput> agents;               ///< Agents in key order.
    std::vector<AppPeerOutput> peers;                 ///< Peers in node-ID order.
};

/** TCP statistics for one peer connection. */
struct TcpConnectionOutput
{
    uint32_t peerNodeId{0};                             ///< Peer node identifier.
    std::string peerIpv4;                               ///< Peer IPv4 address.
    uint64_t congestionWindowObservationDurationUs{0};  ///< Observed CWND duration.
    std::optional<double> averageCongestionWindowBytes; ///< Time-weighted average CWND.
    std::optional<uint64_t> lastCongestionWindowBytes;  ///< Last observed CWND.
    SampleDistributionOutput roundTripTime;             ///< RTT distribution.
};

/** TCP statistics for one direction. */
struct TcpDirectionOutput
{
    std::vector<TcpConnectionOutput> connections; ///< Connections in peer order.
};

/** MAC MPDU drop count for one numeric reason. */
struct MacDropReasonOutput
{
    int reasonCode{0};     ///< Numeric Wi-Fi MAC drop reason.
    uint64_t dropCount{0}; ///< Drops carrying the reason.
};

/** MAC statistics for one peer. */
struct MacPeerOutput
{
    uint32_t peerNodeId{0};                                ///< Peer node identifier.
    std::string peerIpv4;                                  ///< Peer IPv4 address.
    uint64_t estimatedTransmitEventCount{0};               ///< Estimated transmit-event count.
    uint64_t estimatedTransmittedTcpPayloadBytes{0};       ///< Estimated transmitted payload bytes.
    std::optional<double> estimatedTransmitThroughputMbps; ///< Estimated transmit throughput.
    uint64_t estimatedReceiveEventCount{0};                ///< Estimated receive-event count.
    uint64_t estimatedReceivedTcpPayloadBytes{0};          ///< Estimated received payload bytes.
    std::optional<double> estimatedReceiveThroughputMbps;  ///< Estimated receive throughput.
    uint64_t mpduDropCount{0};                             ///< Peer-resolved MPDU drop count.
    uint64_t mpduDropBytes{0};                             ///< Peer-resolved dropped MPDU bytes.
    uint64_t dataFailureCount{0};                          ///< Peer-resolved data failure count.
    uint64_t finalDataFailureCount{0};                     ///< Peer-resolved final failure count.
    std::vector<MacDropReasonOutput> mpduDropsByReason;    ///< Reasons in numeric order.
};

/** MAC statistics for one direction. */
struct MacDirectionOutput
{
    uint64_t estimatedTransmitEventCount{0};               ///< Estimated transmit-event count.
    uint64_t estimatedTransmittedTcpPayloadBytes{0};       ///< Estimated transmitted payload bytes.
    std::optional<double> estimatedTransmitThroughputMbps; ///< Estimated transmit throughput.
    uint64_t estimatedReceiveEventCount{0};                ///< Estimated receive-event count.
    uint64_t estimatedReceivedTcpPayloadBytes{0};          ///< Estimated received payload bytes.
    std::optional<double> estimatedReceiveThroughputMbps;  ///< Estimated receive throughput.
    uint64_t transmitDropCount{0};                         ///< MAC transmit-drop count.
    uint64_t transmitDropPacketBytes{0};                   ///< MAC transmit-drop packet bytes.
    uint64_t mpduDropCount{0};                             ///< Dropped MPDU count.
    uint64_t mpduDropBytes{0};                             ///< Dropped MPDU bytes.
    uint64_t dataFailureCount{0};                          ///< Data failure count.
    uint64_t finalDataFailureCount{0};                     ///< Final data failure count.
    std::vector<MacDropReasonOutput> mpduDropsByReason;    ///< Reasons in numeric order.
    std::vector<MacPeerOutput> peers;                      ///< Peers in node-ID order.
};

/** PHY statistics for one peer. */
struct PhyPeerOutput
{
    uint32_t peerNodeId{0};                    ///< Peer node identifier.
    std::string peerIpv4;                      ///< Peer IPv4 address.
    uint64_t taggedPayloadBytes{0};            ///< Tagged bytes including retransmissions.
    uint64_t uniqueTaggedPayloadBytes{0};      ///< First-transmission tagged bytes.
    uint64_t transmissionAttemptCount{0};      ///< Tagged transmission attempts.
    uint64_t retransmissionCount{0};           ///< Tagged retransmissions.
    double transmissionAirtimeUs{0.0};         ///< Allocated PHY airtime in microseconds.
    std::optional<double> averageDataRateMbps; ///< Airtime-weighted nominal data rate.
    std::optional<double> throughputMbps;      ///< Tagged payload throughput.
};

/** PHY statistics for one direction. */
struct PhyDirectionOutput
{
    uint64_t taggedPayloadBytes{0};            ///< Tagged bytes including retransmissions.
    uint64_t uniqueTaggedPayloadBytes{0};      ///< First-transmission tagged bytes.
    uint64_t taggedMpduCount{0};               ///< Complete tagged MPDU attempt count.
    uint64_t completeTaggedMpduBytes{0};       ///< Complete tagged MPDU bytes.
    uint64_t transmissionAttemptCount{0};      ///< Tagged transmission attempts.
    uint64_t retransmissionCount{0};           ///< Tagged retransmissions.
    double transmissionAirtimeUs{0.0};         ///< Allocated PHY airtime in microseconds.
    std::optional<double> averageDataRateMbps; ///< Airtime-weighted nominal data rate.
    std::optional<double> throughputMbps;      ///< Tagged payload throughput.
    std::vector<PhyPeerOutput> peers;          ///< Peers in node-ID order.
};

/** One ordered station-transmitted data width/NSS/MCS profile. */
struct DataTxProfileOutput
{
    uint16_t channelWidthMhz{0};      ///< Actual data TxVector channel width in MHz.
    uint8_t nss{0};                   ///< Number of spatial streams.
    uint8_t mcs{0};                   ///< HE MCS index.
    double transmittedPsduBytes{0.0}; ///< Attributed transmitted data PSDU bytes.
    uint64_t ppduAttemptCount{0};     ///< PPDU attempts starting in the interval.
    double ppduAirtimeUs{0.0};        ///< Attributed complete-PPDU airtime.
};

/** PHY category statistics including local channel state. */
struct PhyCategoryOutput
{
    std::optional<double> dominantDataPhyRateMbps;      ///< Dominant station data profile rate.
    std::optional<double> dominantDataProfileShare;     ///< Dominant station data byte share.
    std::optional<double> effectivePhyRateMbps;         ///< Data bits per data PPDU airtime.
    std::optional<double> dataTxRateOverIntervalMbps;   ///< Data bits per statistics interval.
    std::optional<double> dataTxOpportunityGapFraction; ///< Time outside data PPDU airtime.
    std::vector<DataTxProfileOutput> dataTxProfile; ///< Profiles in ascending width/NSS/MCS order.
    std::optional<double>
        meanDominantDataPhyRateMbps; ///< BSS mean of defined station dominant rates.
    std::optional<double>
        meanEffectivePhyRateMbps; ///< BSS mean of defined station effective rates.
    std::optional<double>
        aggregateDataTxRateOverIntervalMbps;         ///< BSS sum of station interval rates.
    uint64_t busyTimeUs{0};                          ///< Local PHY busy duration.
    std::optional<double> channelUtilizationPercent; ///< Local channel utilization.
    PhyDirectionOutput uplink;                       ///< Uplink PHY statistics.
    PhyDirectionOutput downlink;                     ///< Downlink PHY statistics.
};

/** Complete fixed category and direction shape for one entity. */
struct EntityStatisticsOutput
{
    DirectionPair<GeneralDirectionOutput> generalStats; ///< General statistics by direction.
    DirectionPair<AppDirectionOutput> appStats;         ///< Application statistics by direction.
    DirectionPair<TcpDirectionOutput> tcpStats;         ///< TCP statistics by direction.
    DirectionPair<MacDirectionOutput> macStats;         ///< MAC statistics by direction.
    PhyCategoryOutput phyStats;                         ///< PHY statistics and local state.
};

/** Statistics and exact identity for one AP BSS parent. */
struct AccessPointStatisticsOutput
{
    uint32_t accessPointId{0};         ///< Zero-based BSS identifier.
    uint32_t nodeId{0};                ///< AP node identifier.
    std::string nodeLabel;             ///< Stable AP label.
    std::string ipv4;                  ///< AP IPv4 address.
    EntityStatisticsOutput statistics; ///< Fixed statistics hierarchy.
};

/** Statistics and exact identity for one station. */
struct StationStatisticsOutput
{
    uint32_t accessPointId{0};         ///< Parent BSS identifier.
    uint32_t stationIndex{0};          ///< Station index within the BSS.
    uint32_t nodeId{0};                ///< Station node identifier.
    std::string nodeLabel;             ///< Stable station label.
    std::string ipv4;                  ///< Station IPv4 address.
    EntityStatisticsOutput statistics; ///< Fixed statistics hierarchy.
};

/** One sparse configured statistics window. */
struct ExperimentWindowOutput
{
    uint64_t windowIndex{0};      ///< Zero-based window index.
    double windowStartMs{0.0};    ///< Start relative to the experiment epoch.
    double windowDurationMs{0.0}; ///< Exact actual window duration.
    std::vector<AccessPointStatisticsOutput> accessPoints; ///< Active AP BSS parents.
    std::vector<StationStatisticsOutput> stations;         ///< Active stations.
};

/** Dense whole-experiment entity statistics. */
struct ExperimentOverallOutput
{
    std::vector<AccessPointStatisticsOutput> accessPoints; ///< Every inventory AP.
    std::vector<StationStatisticsOutput> stations;         ///< Every inventory station.
};

/** Raw-total validation flags for the unified hierarchy. */
struct ExperimentValidationOutput
{
    bool entityInventoryReferencesValid{true};  ///< All entity and peer references are registered.
    bool appAgentTotalsConsistent{true};        ///< Agent totals fit application totals.
    bool appPeerTotalsConsistent{true};         ///< App peer totals fit direction totals.
    bool macPeerTotalsConsistent{true};         ///< MAC peer and reason totals are consistent.
    bool phyPeerTotalsConsistent{true};         ///< PHY peer totals match direction totals.
    bool apStationSenderTotalsConsistent{true}; ///< AP parent sender values match child detail.
    bool overallMatchesWindows{true};           ///< Raw overall equals merged windows.
    bool uniquePhyPayloadWithinTaggedPayload{true}; ///< Unique PHY bytes do not exceed tagged.
};

/** Complete typed unified experiment summary. */
struct UnifiedExperimentSummary
{
    uint32_t statisticsWindowMs{0};              ///< Configured statistics window width.
    std::vector<ExperimentWindowOutput> windows; ///< Sparse configured windows.
    ExperimentOverallOutput overall;             ///< Dense whole-experiment statistics.
    ExperimentValidationOutput validation;       ///< Raw hierarchy validation.
    std::vector<ExperimentEntityIdentity> accessPointInventory; ///< Dense AP inventory.
    std::vector<ExperimentEntityIdentity> stationInventory;     ///< Dense STA inventory.
};

} // namespace ns3

#endif // EXPERIMENT_WINDOW_OUTPUT_H
