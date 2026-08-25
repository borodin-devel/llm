#ifndef WIFI_STATISTICS_INTERNAL_H
#define WIFI_STATISTICS_INTERNAL_H

#include "experiment-statistics-types.h"
#include "wifi-statistics.h"

#include "ns3/abort.h"
#include "ns3/mac48-address.h"
#include "ns3/wifi-phy-band.h"
#include "ns3/wifi-psdu.h"
#include "ns3/wifi-tx-vector.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
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

/** MAC and PHY observations for one fixed time window. */
struct MacWindowStats
{
    std::map<std::string, uint64_t> upBytes;   ///< Uplink bytes by source station.
    std::map<std::string, uint64_t> downBytes; ///< Downlink bytes by destination station.
    std::map<std::string, PhyRateAccumulator> upPhyRates;   ///< Uplink PHY rates by station.
    std::map<std::string, PhyRateAccumulator> downPhyRates; ///< Downlink PHY rates by station.
};

/** Accumulate delay samples for mean and standard-deviation reporting. */
struct DelayAccumulator
{
    uint64_t count{0};             ///< Number of samples.
    long double sumUs{0.0};        ///< Sum of delays in microseconds.
    long double sumSquaresUs{0.0}; ///< Sum of squared delays in microseconds squared.
    double minUs{std::numeric_limits<double>::infinity()}; ///< Minimum delay in microseconds.
    double maxUs{0.0};                                     ///< Maximum delay in microseconds.

    /** @param delayUs Delay sample in microseconds. */
    void Add(double delayUs)
    {
        ++count;
        sumUs += delayUs;
        sumSquaresUs += static_cast<long double>(delayUs) * delayUs;
        minUs = std::min(minUs, delayUs);
        maxUs = std::max(maxUs, delayUs);
    }

    /** @param other Accumulator to merge. */
    void Merge(const DelayAccumulator& other)
    {
        count += other.count;
        sumUs += other.sumUs;
        sumSquaresUs += other.sumSquaresUs;
        minUs = std::min(minUs, other.minUs);
        maxUs = std::max(maxUs, other.maxUs);
    }

    /** @return Mean delay in microseconds. */
    double MeanUs() const
    {
        return count == 0 ? 0.0 : static_cast<double>(sumUs / count);
    }

    /** @return Population standard deviation in microseconds. */
    double StdDevUs() const
    {
        if (count == 0)
        {
            return 0.0;
        }
        const long double mean = sumUs / count;
        const long double variance = std::max<long double>(0.0, sumSquaresUs / count - mean * mean);
        return std::sqrt(static_cast<double>(variance));
    }
};

/** Application drop totals for one agent. */
struct AgentDropStats
{
    uint64_t events{0}; ///< Drop event count.
    uint64_t bytes{0};  ///< Dropped payload bytes.
};

/** Cross-layer observations for one node and absolute second. */
struct NodeSecondStats
{
    DelayAccumulator appToPhy;                             ///< Application-to-PHY delay samples.
    uint64_t appTxEvents{0};                               ///< Application transmit event count.
    uint64_t appTxBytes{0};                                ///< Application transmit payload bytes.
    uint64_t appDropEvents{0};                             ///< Application drop event count.
    uint64_t appDropBytes{0};                              ///< Application-dropped payload bytes.
    std::map<std::string, AgentDropStats> appDropsByAgent; ///< Drops by agent.
    uint64_t phyTaggedMpduCount{0};                        ///< Tagged PHY MPDU count.
    uint64_t phyPayloadBytes{0};                  ///< Tagged PHY payload bytes including retries.
    uint64_t phyUniquePayloadBytes{0};            ///< Deduplicated tagged PHY payload bytes.
    uint64_t phyMpduBytes{0};                     ///< Complete tagged PHY MPDU bytes.
    uint64_t phyRetransmissions{0};               ///< Tagged PHY retransmission count.
    int64_t phyBusyUs{0};                         ///< PHY busy time in microseconds.
    uint64_t macTxDrops{0};                       ///< MAC transmit-drop event count.
    uint64_t macTxDropBytes{0};                   ///< MAC transmit-drop bytes.
    uint64_t macMpduDrops{0};                     ///< MAC MPDU-drop event count.
    uint64_t macMpduDropBytes{0};                 ///< MAC MPDU-drop bytes.
    std::map<int, uint64_t> macMpduDropsByReason; ///< MAC MPDU drops by reason code.
    uint64_t macDataFailures{0};                  ///< MAC data failure count.
    uint64_t macFinalDataFailures{0};             ///< MAC final data failure count.
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

/** All mutable Wi-Fi statistics state for one scenario. */
struct WifiStatisticsState
{
    /**
     * Construct scenario statistics state.
     *
     * @param trafficCoordinator Traffic epoch and duration owner.
     * @param configuredWindowMs Statistics window width in milliseconds.
     */
    WifiStatisticsState(const TrafficCoordinator& trafficCoordinator, uint32_t configuredWindowMs)
        : coordinator(trafficCoordinator),
          windowMs(configuredWindowMs),
          windowUs(static_cast<int64_t>(configuredWindowMs) * 1000)
    {
        NS_ABORT_MSG_IF(windowMs == 0, "Statistics window width must be greater than zero");
    }

    const TrafficCoordinator& coordinator; ///< Traffic epoch and duration owner.
    const uint32_t windowMs;               ///< Statistics window width in milliseconds.
    const int64_t windowUs;                ///< Statistics window width in microseconds.
    std::vector<std::vector<std::string>> stationIpsByBss;        ///< Station IPs by BSS.
    std::map<std::string, int> bssByApIp;                         ///< BSS index by AP IP.
    std::map<std::string, int> bssByStationIp;                    ///< BSS index by station IP.
    std::map<uint64_t, std::map<int, MacWindowStats>> macWindows; ///< MAC windows by node.
    std::map<uint64_t, std::map<int, MacWindowStats>> phyWindows; ///< PHY windows by node.
    std::map<uint32_t, std::map<uint64_t, NodeSecondStats>> nodeSeconds; ///< Node seconds.
    std::map<uint32_t, std::string> nodeLabels;                          ///< Report label by node.
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

/**
 * Resolve an event to one configured statistics window.
 *
 * @param statistics Scenario statistics state.
 * @param absoluteTimeUs Absolute event time in microseconds.
 * @param bounds Resolved window bounds.
 * @return True when the event lies inside the experiment interval.
 */
bool ResolveStatisticsEventWindow(const WifiStatisticsState& statistics,
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
void RecordPhyMpduAttempt(WifiStatisticsState& statistics,
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
void RecordPhyTxBeginPacket(WifiStatisticsState& statistics,
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
void RecordPhyRateAttempt(WifiStatisticsState& statistics,
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
void RecordPhyTxPsduBegin(WifiStatisticsState& statistics,
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
void RecordPhyBusyInterval(WifiStatisticsState& statistics,
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
void ConnectPhyTraces(WifiStatisticsState& statistics, uint32_t nodeId, Ptr<WifiNetDevice> device);

/**
 * Record one parsed device transmit observation.
 *
 * @param statistics Scenario statistics state.
 * @param absoluteTimeUs Absolute transmit time in microseconds.
 * @param payload Parsed TCP payload observation.
 */
void RecordParsedDeviceTransmit(WifiStatisticsState& statistics,
                                int64_t absoluteTimeUs,
                                const ParsedDeviceTcpPayload& payload);

/**
 * Record one parsed device receive observation.
 *
 * @param statistics Scenario statistics state.
 * @param absoluteTimeUs Absolute receive time in microseconds.
 * @param payload Parsed TCP payload observation.
 */
void RecordParsedDeviceReceive(WifiStatisticsState& statistics,
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
bool RecordDeviceTransmitPacket(WifiStatisticsState& statistics,
                                int64_t absoluteTimeUs,
                                Ptr<const Packet> packet);

/**
 * Finalize positive ordered device transmit/receive matches.
 *
 * @param statistics Scenario statistics state.
 */
void FinalizeDeviceStatistics(WifiStatisticsState& statistics);

/**
 * Build the transitional transmission summary from central device state.
 *
 * @param statistics Scenario statistics state.
 * @return Per-sender transmission measurements.
 */
TransmissionSummary BuildTransmissionSummary(WifiStatisticsState& statistics);

/**
 * Record one MAC transmit drop in configured and legacy windows.
 *
 * @param statistics Scenario statistics state.
 * @param nodeId Local transmitter node identifier.
 * @param absoluteTimeUs Absolute event time in microseconds.
 * @param packetBytes Complete dropped packet bytes.
 */
void RecordMacTransmitDrop(WifiStatisticsState& statistics,
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
void RecordMacMpduDrop(WifiStatisticsState& statistics,
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
void RecordMacDataFailure(WifiStatisticsState& statistics,
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
void ConnectMacTraces(WifiStatisticsState& statistics, uint32_t nodeId, Ptr<WifiNetDevice> device);

/**
 * Attribute one MAC payload to a fixed statistics window.
 *
 * @param statistics Scenario statistics state.
 * @param windowIndex Zero-based statistics-window index.
 * @param sourceIp Source IPv4 address.
 * @param destinationIp Destination IPv4 address.
 * @param payloadBytes Application payload size in bytes.
 * @return True when the flow belongs to one registered BSS.
 */
bool RecordMacPayloadInWindow(WifiStatisticsState& statistics,
                              uint64_t windowIndex,
                              const std::string& sourceIp,
                              const std::string& destinationIp,
                              uint32_t payloadBytes);

/**
 * Resolve a relative timestamp to an absolute-second statistics index.
 *
 * @param relativeUs Timestamp relative to the experiment start in microseconds.
 * @param experimentDurationUs Experiment duration in microseconds.
 * @param secondIndex Resolved zero-based second index.
 * @return True when the timestamp lies inside the experiment interval.
 */
bool GetNodeSecondIndex(int64_t relativeUs, int64_t experimentDurationUs, uint64_t& secondIndex);

/**
 * Build typed cross-layer measurements from collected trace data.
 *
 * @param statistics Scenario statistics state.
 * @return Per-node interval and overall cross-layer measurements.
 */
CrossLayerSummary BuildCrossLayerSummary(const WifiStatisticsState& statistics);

} // namespace ns3

#endif // WIFI_STATISTICS_INTERNAL_H
