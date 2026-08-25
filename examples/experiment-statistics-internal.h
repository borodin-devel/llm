#ifndef EXPERIMENT_STATISTICS_INTERNAL_H
#define EXPERIMENT_STATISTICS_INTERNAL_H

#include "experiment-statistics-types.h"
#include "experiment-statistics.h"

#include "ns3/abort.h"
#include "ns3/mac48-address.h"
#include "ns3/wifi-phy-band.h"
#include "ns3/wifi-psdu.h"
#include "ns3/wifi-tx-vector.h"

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace ns3
{

class TrafficCoordinator;
class Packet;
class WifiNetDevice;

/** Parsed IPv4/TCP payload observation from a Wi-Fi device trace. */
struct ParsedDeviceTcpPayload
{
    std::string sourceIpv4;         ///< Source IPv4 address.
    uint16_t sourcePort;            ///< Source TCP port.
    std::string destinationIpv4;    ///< Destination IPv4 address.
    uint16_t destinationPort;       ///< Destination TCP port.
    uint32_t estimatedPayloadBytes; ///< Estimated TCP payload size in bytes.
};

/** Key identifying one parsed device TCP payload flow. */
struct DeviceFlowKey
{
    std::string sourceIpv4;         ///< Source IPv4 address.
    uint16_t sourcePort;            ///< Source TCP port.
    std::string destinationIpv4;    ///< Destination IPv4 address.
    uint16_t destinationPort;       ///< Destination TCP port.
    uint32_t estimatedPayloadBytes; ///< Estimated TCP payload size in bytes.

    /**
     * Compare flow identities for deterministic map ordering.
     *
     * @param other Flow identity to compare.
     * @return True when this flow sorts before @p other.
     */
    bool operator<(const DeviceFlowKey& other) const;
};

/** Sender-side state retained for one device transmit observation. */
struct DeviceTransmitObservation
{
    int64_t absoluteTimeUs;        ///< Absolute transmit time in microseconds.
    uint64_t windowIndex;          ///< Causal transmit window index.
    uint32_t senderNodeId;         ///< Registered sender node identifier.
    ExperimentDirection direction; ///< Traffic direction at the sender.
};

/** Identity used to distinguish first tagged MPDU transmissions from retries. */
using PhyMpduKey = std::tuple<uint32_t, std::string, std::string, uint16_t, uint8_t, uint64_t>;

/** One application-tagged byte span observed in a transmitted MPDU. */
struct PhyTaggedPayloadSpan
{
    std::string sourceIpv4;            ///< Tagged source IPv4 address.
    std::string destinationIpv4;       ///< Tagged destination IPv4 address.
    int64_t applicationTransmitTimeUs; ///< Original application transmit time in microseconds.
    uint32_t bytes;                    ///< Number of bytes covered by the tag.
};

/** Identity of one application receive stream across experiment windows. */
struct AppReceiveStreamKey
{
    uint32_t nodeId;                    ///< Local receiving node identifier.
    ExperimentDirection direction;      ///< Traffic direction at the local node.
    std::optional<uint32_t> peerNodeId; ///< Remote peer node identifier when resolved.

    /** @return True when this key sorts before @p other. */
    bool operator<(const AppReceiveStreamKey& other) const;
};

/** All mutable experiment statistics state for one scenario. */
struct ExperimentStatisticsState
{
    /**
     * Construct scenario statistics state.
     *
     * @param trafficCoordinator Traffic epoch and duration owner.
     * @param configuredWindowMs Statistics window width in milliseconds.
     */
    ExperimentStatisticsState(const TrafficCoordinator& trafficCoordinator,
                              uint32_t configuredWindowMs)
        : coordinator(trafficCoordinator),
          windowMs(configuredWindowMs),
          windowUs(static_cast<int64_t>(configuredWindowMs) * 1000)
    {
        NS_ABORT_MSG_IF(windowMs == 0, "Statistics window width must be greater than zero");
    }

    const TrafficCoordinator& coordinator;       ///< Traffic epoch and duration owner.
    const uint32_t windowMs;                     ///< Statistics window width in milliseconds.
    const int64_t windowUs;                      ///< Statistics window width in microseconds.
    std::set<PhyMpduKey> seenTaggedMpdus;        ///< Tagged MPDUs already counted as unique.
    ExperimentEntityRegistry entityRegistry;     ///< Registered AP and station identities.
    UnifiedExperimentWindowStore unifiedWindows; ///< Sparse unified experiment windows.
    std::map<AppReceiveStreamKey, int64_t>
        lastApplicationReceiveTimeUs; ///< Last receive time by local direction and peer.
    std::map<TcpConnectionKey, TcpConnectionState>
        tcpConnectionStates;            ///< Current CWND step state by TCP connection.
    bool tcpStatisticsFinalized{false}; ///< Whether TCP states were flushed through experiment end.
    std::map<DeviceFlowKey, std::vector<DeviceTransmitObservation>>
        deviceTransmitsByFlow; ///< Ordered device transmit observations by flow.
    std::map<DeviceFlowKey, std::vector<int64_t>>
        deviceReceivesByFlow; ///< Ordered device receive timestamps by flow.
    std::map<Mac48Address, uint32_t> nodeIdsByMacAddress; ///< Registered node IDs by MAC address.
    bool deviceStatisticsFinalized{false}; ///< Whether device matches were accumulated.
    bool deviceTracesConnected{false};     ///< Whether global device traces were connected.
};

/** Raw materialized local, AP-parent, station, and overall summary state. */
struct UnifiedSummaryRawState
{
    UnifiedExperimentWindowStore localWindows;       ///< Original local windows.
    UnifiedExperimentWindowStore accessPointWindows; ///< Materialized AP BSS-parent windows.
    UnifiedExperimentWindowStore stationWindows;     ///< Materialized station windows.
    UnifiedEntityAccumulatorMap accessPointOverall;  ///< Dense raw AP overall values.
    UnifiedEntityAccumulatorMap stationOverall;      ///< Dense raw station overall values.
};

/**
 * Merge every raw field from one entity accumulator.
 *
 * @param target Destination accumulator.
 * @param source Source accumulator.
 */
void MergeLocalEntityWindowAccumulator(LocalEntityWindowAccumulator& target,
                                       const LocalEntityWindowAccumulator& source);

/**
 * Merge one station's applicable sender/receiver values into its AP BSS parent.
 *
 * @param target Parent AP accumulator.
 * @param station Child station accumulator.
 * @param accessPointNodeId Parent AP node identifier.
 * @param stationNodeId Child station node identifier.
 */
void MergeStationIntoAccessPoint(LocalEntityWindowAccumulator& target,
                                 const LocalEntityWindowAccumulator& station,
                                 uint32_t accessPointNodeId,
                                 uint32_t stationNodeId);

/**
 * Test whether an entity has any raw activity.
 *
 * @param accumulator Raw entity accumulator.
 * @return True when at least one measurement is present.
 */
bool HasEntityActivity(const LocalEntityWindowAccumulator& accumulator);

/**
 * Materialize AP parents, station children, and dense raw overall values.
 *
 * @param statistics Scenario statistics state.
 * @return Independently copyable raw summary state.
 */
UnifiedSummaryRawState BuildUnifiedSummaryRawState(const ExperimentStatisticsState& statistics);

/**
 * Finalize one raw entity into the fixed output hierarchy.
 *
 * @param accumulator Raw entity accumulator.
 * @param durationUs Rate and utilization denominator in microseconds.
 * @param statistics Scenario statistics state used for peer identities.
 * @return Fixed entity output hierarchy.
 */
EntityStatisticsOutput FinalizeEntityStatistics(const LocalEntityWindowAccumulator& accumulator,
                                                int64_t durationUs,
                                                const ExperimentStatisticsState& statistics);

/**
 * Validate exact raw summary invariants.
 *
 * @param registry Complete entity registry.
 * @param raw Independently materialized raw summary state.
 * @return Eight raw-total validation flags.
 */
ExperimentValidationOutput ValidateUnifiedSummaryRawState(const ExperimentEntityRegistry& registry,
                                                          const UnifiedSummaryRawState& raw);

/**
 * Resolve an event to one configured statistics window.
 *
 * @param statistics Scenario statistics state.
 * @param absoluteTimeUs Absolute event time in microseconds.
 * @param bounds Resolved window bounds.
 * @return True when the event lies inside the experiment interval.
 */
bool ResolveStatisticsEventWindow(const ExperimentStatisticsState& statistics,
                                  int64_t absoluteTimeUs,
                                  ExperimentWindowBounds& bounds);

/**
 * Record one tagged MPDU transmission attempt in configured and legacy windows.
 *
 * @param statistics Scenario statistics state.
 * @param transmitterNodeId Local PHY transmitter node identifier.
 * @param absoluteTimeUs Absolute attempt time in microseconds.
 * @param completeMpduBytes Complete transmitted MPDU size in bytes.
 * @param spans Application-tagged payload spans in the MPDU.
 * @param identity Stable MPDU identity when the Wi-Fi data header is available.
 */
void RecordPhyMpduAttempt(ExperimentStatisticsState& statistics,
                          uint32_t transmitterNodeId,
                          int64_t absoluteTimeUs,
                          uint32_t completeMpduBytes,
                          const std::vector<PhyTaggedPayloadSpan>& spans,
                          const std::optional<PhyMpduKey>& identity);

/**
 * Parse and record one packet delivered to the PhyTxBegin trace.
 *
 * @param statistics Scenario statistics state.
 * @param transmitterNodeId Local PHY transmitter node identifier.
 * @param absoluteTimeUs Absolute attempt time in microseconds.
 * @param packet Complete transmitted Wi-Fi packet.
 */
void RecordPhyTxBeginPacket(ExperimentStatisticsState& statistics,
                            uint32_t transmitterNodeId,
                            int64_t absoluteTimeUs,
                            Ptr<const Packet> packet);

/**
 * Record one grouped tagged-flow contribution to a PPDU attempt.
 *
 * @param statistics Scenario statistics state.
 * @param transmitterNodeId Local PHY transmitter node identifier.
 * @param absoluteTimeUs Absolute attempt time in microseconds.
 * @param sourceIpv4 Tagged source IPv4 address.
 * @param destinationIpv4 Tagged destination IPv4 address.
 * @param dataRateBps Byte-weighted PHY data rate in bits per second.
 * @param allocatedAirtimeUs PPDU airtime allocated to the tagged flow in microseconds.
 */
void RecordPhyRateAttempt(ExperimentStatisticsState& statistics,
                          uint32_t transmitterNodeId,
                          int64_t absoluteTimeUs,
                          const std::string& sourceIpv4,
                          const std::string& destinationIpv4,
                          double dataRateBps,
                          long double allocatedAirtimeUs);

/**
 * Parse and record one PSDU map delivered to the PhyTxPsduBegin trace.
 *
 * @param statistics Scenario statistics state.
 * @param transmitterNodeId Local PHY transmitter node identifier.
 * @param absoluteTimeUs Absolute attempt time in microseconds.
 * @param band PHY band used by the transmitter.
 * @param psduMap Transmitted PSDUs indexed by STA ID.
 * @param txVector Transmission parameters, including per-user modes.
 */
void RecordPhyTxPsduBegin(ExperimentStatisticsState& statistics,
                          uint32_t transmitterNodeId,
                          int64_t absoluteTimeUs,
                          WifiPhyBand band,
                          const WifiConstPsduMap& psduMap,
                          const WifiTxVector& txVector);

/**
 * Calculate the airtime-weighted data rate for one PHY accumulator.
 *
 * @param accumulator PHY accumulator to summarize.
 * @return Average data rate in megabits per second, or null without positive airtime.
 */
std::optional<double> CalculateAveragePhyDataRateMbps(const PhyPeerAccumulator& accumulator);

/**
 * Record one local PHY busy interval in configured and legacy windows.
 *
 * @param statistics Scenario statistics state.
 * @param nodeId Local PHY node identifier.
 * @param absoluteStartUs Absolute interval start in microseconds.
 * @param durationUs Interval duration in microseconds.
 */
void RecordPhyBusyInterval(ExperimentStatisticsState& statistics,
                           uint32_t nodeId,
                           int64_t absoluteStartUs,
                           int64_t durationUs);

/**
 * Connect all PHY traces for one Wi-Fi device.
 *
 * @param statistics Scenario statistics state.
 * @param nodeId Owning node identifier.
 * @param device Wi-Fi device whose PHY traces are connected.
 */
void ConnectPhyTraces(ExperimentStatisticsState& statistics,
                      uint32_t nodeId,
                      Ptr<WifiNetDevice> device);

/**
 * Record one parsed device transmit observation.
 *
 * @param statistics Scenario statistics state.
 * @param absoluteTimeUs Absolute transmit time in microseconds.
 * @param payload Parsed TCP payload observation.
 */
void RecordParsedDeviceTransmit(ExperimentStatisticsState& statistics,
                                int64_t absoluteTimeUs,
                                const ParsedDeviceTcpPayload& payload);

/**
 * Record one parsed device receive observation.
 *
 * @param statistics Scenario statistics state.
 * @param absoluteTimeUs Absolute receive time in microseconds.
 * @param payload Parsed TCP payload observation.
 */
void RecordParsedDeviceReceive(ExperimentStatisticsState& statistics,
                               int64_t absoluteTimeUs,
                               const ParsedDeviceTcpPayload& payload);

/**
 * Parse and record one device transmit packet.
 *
 * @param statistics Scenario statistics state.
 * @param absoluteTimeUs Absolute transmit time in microseconds.
 * @param packet Wi-Fi MAC payload packet.
 * @return True when the packet contains a supported IPv4/TCP payload.
 */
bool RecordDeviceTransmitPacket(ExperimentStatisticsState& statistics,
                                int64_t absoluteTimeUs,
                                Ptr<const Packet> packet);

/**
 * Finalize positive ordered device transmit/receive matches.
 *
 * @param statistics Scenario statistics state.
 */
void FinalizeDeviceStatistics(ExperimentStatisticsState& statistics);

/**
 * Record one MAC transmit drop in configured and legacy windows.
 *
 * @param statistics Scenario statistics state.
 * @param nodeId Local transmitter node identifier.
 * @param absoluteTimeUs Absolute event time in microseconds.
 * @param packetBytes Complete dropped packet bytes.
 */
void RecordMacTransmitDrop(ExperimentStatisticsState& statistics,
                           uint32_t nodeId,
                           int64_t absoluteTimeUs,
                           uint32_t packetBytes);

/**
 * Record one MAC MPDU drop in configured and legacy windows.
 *
 * @param statistics Scenario statistics state.
 * @param nodeId Local transmitter node identifier.
 * @param absoluteTimeUs Absolute event time in microseconds.
 * @param reasonCode Numeric Wi-Fi MAC drop reason.
 * @param mpduBytes Complete dropped MPDU bytes.
 * @param peerNodeId Remote peer node identifier when resolved.
 */
void RecordMacMpduDrop(ExperimentStatisticsState& statistics,
                       uint32_t nodeId,
                       int64_t absoluteTimeUs,
                       int reasonCode,
                       uint32_t mpduBytes,
                       std::optional<uint32_t> peerNodeId = std::nullopt);

/**
 * Record one MAC data failure in configured and legacy windows.
 *
 * @param statistics Scenario statistics state.
 * @param nodeId Local transmitter node identifier.
 * @param absoluteTimeUs Absolute event time in microseconds.
 * @param finalFailure Whether this is a final data failure.
 * @param peerNodeId Remote peer node identifier when resolved.
 */
void RecordMacDataFailure(ExperimentStatisticsState& statistics,
                          uint32_t nodeId,
                          int64_t absoluteTimeUs,
                          bool finalFailure,
                          std::optional<uint32_t> peerNodeId = std::nullopt);

/**
 * Connect per-device MAC drop and failure traces.
 *
 * @param statistics Scenario statistics state.
 * @param nodeId Owning node identifier.
 * @param device Wi-Fi device whose traces are connected.
 */
void ConnectMacTraces(ExperimentStatisticsState& statistics,
                      uint32_t nodeId,
                      Ptr<WifiNetDevice> device);

} // namespace ns3

#endif // EXPERIMENT_STATISTICS_INTERNAL_H
