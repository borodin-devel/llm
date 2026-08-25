#ifndef EXPERIMENT_STATISTICS_TYPES_H
#define EXPERIMENT_STATISTICS_TYPES_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ns3
{

/** Kinds of entities represented in the experiment inventory. */
enum class ExperimentEntityKind
{
    ACCESS_POINT,
    STATION
};

/** Directions used by experiment measurements. */
enum class ExperimentDirection
{
    UPLINK,
    DOWNLINK
};

/** Stable identity for one access point or station. */
struct ExperimentEntityIdentity
{
    ExperimentEntityKind kind;            ///< Entity kind.
    uint32_t accessPointId;               ///< Zero-based BSS identifier.
    std::optional<uint32_t> stationIndex; ///< Zero-based station index within the BSS.
    uint32_t nodeId;                      ///< ns-3 node identifier.
    std::string nodeLabel;                ///< Stable entity label.
    std::string ipv4;                     ///< Entity IPv4 address.
};

/** Bounds of one configured experiment window. */
struct ExperimentWindowBounds
{
    uint64_t index;     ///< Zero-based window index.
    int64_t startUs;    ///< Start time relative to the experiment epoch in microseconds.
    int64_t durationUs; ///< Exact window duration in microseconds.
};

/** Accumulate samples for later aggregate statistics. */
struct SampleAccumulator
{
    uint64_t count{0};                                       ///< Number of samples.
    long double sum{0.0};                                    ///< Sum of samples.
    long double sumSquares{0.0};                             ///< Sum of squared samples.
    double minimum{std::numeric_limits<double>::infinity()}; ///< Minimum sample value.
    double maximum{0.0};                                     ///< Maximum sample value.

    /**
     * Add one sample.
     *
     * @param value Sample value.
     */
    void Add(double value);

    /**
     * Merge another accumulator.
     *
     * @param other Accumulator to merge.
     */
    void Merge(const SampleAccumulator& other);
};

/** Pair of values separated by traffic direction. */
template <typename T>
struct DirectionPair
{
    T uplink;   ///< Uplink value.
    T downlink; ///< Downlink value.

    /**
     * Get the value for one direction.
     *
     * @param direction Requested direction.
     * @return Mutable directional value.
     */
    T& Get(ExperimentDirection direction)
    {
        return direction == ExperimentDirection::UPLINK ? uplink : downlink;
    }

    /**
     * Get the value for one direction.
     *
     * @param direction Requested direction.
     * @return Directional value.
     */
    const T& Get(ExperimentDirection direction) const
    {
        return direction == ExperimentDirection::UPLINK ? uplink : downlink;
    }
};

/** Application send and drop totals for one agent. */
struct AppAgentAccumulator
{
    uint64_t acceptedSendCount{0};    ///< Accepted send-event count.
    uint64_t acceptedPayloadBytes{0}; ///< Payload bytes accepted by the socket.
    uint64_t dropEventCount{0};       ///< Application drop-event count.
    uint64_t droppedPayloadBytes{0};  ///< Payload bytes rejected by the socket.
};

/** Application send, receive, and drop totals for one peer node. */
struct AppPeerAccumulator
{
    uint64_t acceptedSendCount{0};    ///< Accepted send-event count.
    uint64_t acceptedPayloadBytes{0}; ///< Payload bytes accepted by the socket.
    uint64_t receiveEventCount{0};    ///< Sink receive-event count.
    uint64_t receivedPayloadBytes{0}; ///< Payload bytes received by the sink.
    uint64_t dropEventCount{0};       ///< Application drop-event count.
    uint64_t droppedPayloadBytes{0};  ///< Payload bytes rejected by the socket.
};

/** Application totals for one local entity and traffic direction. */
struct AppDirectionAccumulator
{
    uint64_t acceptedSendCount{0};           ///< Accepted send-event count.
    uint64_t acceptedPayloadBytes{0};        ///< Payload bytes accepted by the socket.
    uint64_t receiveEventCount{0};           ///< Sink receive-event count.
    uint64_t receivedPayloadBytes{0};        ///< Payload bytes received by the sink.
    uint64_t dropEventCount{0};              ///< Application drop-event count.
    uint64_t droppedPayloadBytes{0};         ///< Payload bytes rejected by the socket.
    SampleAccumulator receiveInterArrivalUs; ///< Receive inter-arrival samples in microseconds.
    std::map<std::string, AppAgentAccumulator> agents;    ///< Agent totals ordered by key.
    std::map<uint32_t, AppPeerAccumulator> peersByNodeId; ///< Peer totals ordered by node ID.
};

/** Device transmission observations for one entity and direction. */
struct DeviceTransmissionAccumulator
{
    uint64_t estimatedTransmittedTcpPayloadBytes{0}; ///< Estimated transmitted TCP payload bytes.
    uint64_t estimatedMatchedTcpPayloadBytes{0}; ///< Estimated positively matched payload bytes.
    uint64_t matchedPacketCount{0};              ///< Positive-duration matched packet count.
    SampleAccumulator transmissionDurationUs;    ///< Positive transmission durations in us.
};

/** MAC payload observations for one peer. */
struct MacPeerAccumulator
{
    uint64_t estimatedTransmitEventCount{0}; ///< Estimated TCP payload transmit-event count.
    uint64_t estimatedTransmittedTcpPayloadBytes{0}; ///< Estimated transmitted TCP payload bytes.
    uint64_t estimatedReceiveEventCount{0};          ///< Estimated TCP payload receive-event count.
    uint64_t estimatedReceivedTcpPayloadBytes{0};    ///< Estimated received TCP payload bytes.
    uint64_t mpduDropCount{0};                       ///< Dropped MPDU count.
    uint64_t mpduDropBytes{0};                       ///< Complete dropped MPDU bytes.
    std::map<int, uint64_t> mpduDropsByReason;       ///< MPDU drops ordered by numeric reason.
    uint64_t dataFailureCount{0};                    ///< MAC data transmission failure count.
    uint64_t finalDataFailureCount{0};               ///< MAC final data transmission failure count.
};

/** MAC observations for one entity and direction. */
struct MacDirectionAccumulator
{
    uint64_t estimatedTransmitEventCount{0}; ///< Estimated TCP payload transmit-event count.
    uint64_t estimatedTransmittedTcpPayloadBytes{0}; ///< Estimated transmitted TCP payload bytes.
    uint64_t estimatedReceiveEventCount{0};          ///< Estimated TCP payload receive-event count.
    uint64_t estimatedReceivedTcpPayloadBytes{0};    ///< Estimated received TCP payload bytes.
    uint64_t transmitDropCount{0};                   ///< MAC transmit-drop event count.
    uint64_t transmitDropPacketBytes{0};             ///< Complete packets rejected by MAC transmit.
    uint64_t mpduDropCount{0};                       ///< Dropped MPDU count.
    uint64_t mpduDropBytes{0};                       ///< Complete dropped MPDU bytes.
    uint64_t dataFailureCount{0};                    ///< MAC data transmission failure count.
    uint64_t finalDataFailureCount{0};               ///< MAC final data transmission failure count.
    std::map<int, uint64_t> mpduDropsByReason;       ///< MPDU drops ordered by numeric reason.
    std::map<uint32_t, MacPeerAccumulator> peersByNodeId; ///< Payload totals by peer node.
};

/** PHY observations for one peer in one experiment window. */
struct PhyPeerAccumulator
{
    uint64_t taggedPayloadBytes{0};         ///< Tagged payload bytes including retransmissions.
    uint64_t uniqueTaggedPayloadBytes{0};   ///< Tagged payload bytes from first transmissions.
    uint64_t transmissionAttemptCount{0};   ///< Tagged PPDU transmission attempts.
    uint64_t retransmissionCount{0};        ///< Repeated tagged MPDU identities.
    long double dataRateBpsUs{0.0};         ///< PHY data rate multiplied by allocated airtime.
    long double transmissionAirtimeUs{0.0}; ///< Allocated transmission airtime in microseconds.
};

/** PHY observations for one entity and traffic direction. */
struct PhyDirectionAccumulator : PhyPeerAccumulator
{
    uint64_t taggedMpduCount{0};                          ///< Complete tagged MPDU attempt count.
    uint64_t completeTaggedMpduBytes{0};                  ///< Complete tagged MPDU attempt bytes.
    std::map<uint32_t, PhyPeerAccumulator> peersByNodeId; ///< PHY totals by peer node.
};

/** PHY observations for one entity in one experiment window. */
struct PhyCategoryAccumulator
{
    int64_t busyTimeUs{0};            ///< Local PHY busy time in microseconds.
    PhyDirectionAccumulator uplink;   ///< Uplink PHY observations.
    PhyDirectionAccumulator downlink; ///< Downlink PHY observations.
};

/** Stable identity of one owner-local TCP connection. */
struct TcpConnectionKey
{
    uint32_t ownerNodeId;          ///< Local owner node identifier.
    ExperimentDirection direction; ///< Traffic direction at the owner.
    uint32_t peerNodeId;           ///< Remote peer node identifier.

    /**
     * Compare connection identities for deterministic map ordering.
     *
     * @param other Connection identity to compare.
     * @return True when this key sorts before @p other.
     */
    bool operator<(const TcpConnectionKey& other) const;
};

/** TCP observations for one peer in one experiment window. */
struct TcpWindowAccumulator
{
    long double congestionWindowBytesUs{0.0};          ///< CWND bytes multiplied by duration.
    uint64_t congestionWindowObservationDurationUs{0}; ///< Observed CWND duration.
    std::optional<uint32_t> lastCongestionWindowBytes; ///< Terminal observed CWND.
    SampleAccumulator roundTripTimeUs;                 ///< RTT samples in microseconds.
};

/** Current step-function state for one TCP connection. */
struct TcpConnectionState
{
    std::optional<uint32_t> currentCongestionWindowBytes; ///< Current CWND in bytes.
    int64_t stateStartUs{0}; ///< Absolute time at which the current state began.
};

/** Per-entity accumulator for one unified experiment window. */
struct LocalEntityWindowAccumulator
{
    DirectionPair<AppDirectionAccumulator> app; ///< Application observations by direction.
    DirectionPair<DeviceTransmissionAccumulator>
        deviceTransmission; ///< Device transmission observations by direction.
    DirectionPair<SampleAccumulator>
        applicationToPhyDelayUs;                ///< Application-to-PHY delay by sender direction.
    DirectionPair<MacDirectionAccumulator> mac; ///< MAC observations by direction.
    PhyCategoryAccumulator phy;                 ///< PHY observations and local busy time.
    std::map<std::pair<ExperimentDirection, uint32_t>, TcpWindowAccumulator>
        tcpConnections; ///< TCP observations by direction and peer node.
};

/** Sparse per-window state keyed by entity node identifier. */
using UnifiedExperimentWindowStore =
    std::map<uint64_t, std::map<uint32_t, LocalEntityWindowAccumulator>>;

/** Register and resolve stable experiment entity identities. */
class ExperimentEntityRegistry
{
  public:
    /**
     * Register one access point identity.
     *
     * @param accessPointId Zero-based BSS identifier.
     * @param nodeId ns-3 node identifier.
     * @param nodeLabel Stable entity label.
     * @param ipv4 Entity IPv4 address.
     * @throws std::invalid_argument if the identity, node ID, or IPv4 address is duplicated.
     */
    void RegisterAccessPoint(uint32_t accessPointId,
                             uint32_t nodeId,
                             std::string nodeLabel,
                             std::string ipv4);

    /**
     * Register one station identity.
     *
     * @param accessPointId Parent zero-based BSS identifier.
     * @param stationIndex Zero-based station index within the BSS.
     * @param nodeId ns-3 node identifier.
     * @param nodeLabel Stable entity label.
     * @param ipv4 Entity IPv4 address.
     * @throws std::invalid_argument if the identity, node ID, or IPv4 address is duplicated.
     */
    void RegisterStation(uint32_t accessPointId,
                         uint32_t stationIndex,
                         uint32_t nodeId,
                         std::string nodeLabel,
                         std::string ipv4);

    /**
     * Find an entity by node identifier.
     *
     * @param nodeId ns-3 node identifier.
     * @return Entity identity, or null when the node is not registered.
     */
    const ExperimentEntityIdentity* FindByNodeId(uint32_t nodeId) const;

    /**
     * Find an entity by IPv4 address.
     *
     * @param ipv4 Entity IPv4 address.
     * @return Entity identity, or null when the address is not registered.
     */
    const ExperimentEntityIdentity* FindByIpv4(std::string_view ipv4) const;

    /**
     * Get all access point identities in access point ID order.
     *
     * @return Deterministically ordered access point identities.
     */
    const std::vector<ExperimentEntityIdentity>& GetAccessPoints() const;

    /**
     * Get all station identities in parent AP and station-index order.
     *
     * @return Deterministically ordered station identities.
     */
    const std::vector<ExperimentEntityIdentity>& GetStations() const;

  private:
    /** Location of an identity in one deterministic entity array. */
    struct EntityLocation
    {
        bool isAccessPoint; ///< Whether the location refers to the AP array.
        std::size_t index;  ///< Index within the selected entity array.
    };

    /** Rebuild lookup locations after deterministic ordering. */
    void RebuildLookupIndexes();

    std::vector<ExperimentEntityIdentity> m_accessPoints; ///< AP identities by access point ID.
    std::vector<ExperimentEntityIdentity> m_stations;     ///< Station identities by AP and station.
    std::map<uint32_t, std::size_t> m_accessPointIndexes; ///< AP vector index by access point ID.
    std::map<std::pair<uint32_t, uint32_t>, std::size_t>
        m_stationIndexes;                               ///< Station vector index by identity.
    std::map<uint32_t, EntityLocation> m_nodeLocations; ///< Entity location by node ID.
    std::map<std::string, EntityLocation, std::less<>>
        m_ipv4Locations; ///< Entity location by IPv4 address.
};

/**
 * Resolve a relative timestamp to one experiment window.
 *
 * @param relativeUs Timestamp relative to the experiment epoch in microseconds.
 * @param experimentDurationUs Exact experiment duration in microseconds.
 * @param windowUs Configured window width in microseconds.
 * @param bounds Resolved window bounds on success.
 * @return True when the timestamp lies inside the experiment interval.
 */
bool ResolveExperimentWindow(int64_t relativeUs,
                             int64_t experimentDurationUs,
                             int64_t windowUs,
                             ExperimentWindowBounds& bounds);

/**
 * Split an interval across the configured experiment windows.
 *
 * @param relativeStartUs Interval start relative to the experiment epoch in microseconds.
 * @param relativeEndUs Interval end relative to the experiment epoch in microseconds.
 * @param experimentDurationUs Exact experiment duration in microseconds.
 * @param windowUs Configured window width in microseconds.
 * @return Ordered pairs of window index and overlap duration in microseconds.
 */
std::vector<std::pair<uint64_t, int64_t>> SplitExperimentInterval(int64_t relativeStartUs,
                                                                  int64_t relativeEndUs,
                                                                  int64_t experimentDurationUs,
                                                                  int64_t windowUs);

} // namespace ns3

#endif // EXPERIMENT_STATISTICS_TYPES_H
