#ifndef WIFI_STATISTICS_H
#define WIFI_STATISTICS_H

#include "experiment-output.h"
#include "experiment-statistics-types.h"
#include "experiment-window-output.h"

#include "ns3/address.h"
#include "ns3/ipv4-address.h"
#include "ns3/ptr.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

class ExperimentJsonTestCase;
class ExperimentAppTestCase;
class ExperimentTcpTestCase;
class ExperimentSummaryTestCase;
class ExperimentOverallTestCase;
class ExperimentValidationTestCase;

namespace ns3
{

class APGenerator;
class Ipv4InterfaceContainer;
class NetDevice;
class StaLlmGenerator;
class Time;
class TrafficCoordinator;
class TrafficSink;
struct ScenarioConfig;
struct WifiStatisticsState;

/** Accumulate PHY rates weighted by allocated airtime. */
struct PhyRateAccumulator
{
    uint64_t txAttempts{0};             ///< Number of tagged transmit attempts.
    long double weightedRateBpsUs{0.0}; ///< Sum of rate multiplied by airtime.
    long double airtimeUs{0.0};         ///< Allocated airtime in microseconds.

    /**
     * Add one rate sample.
     *
     * @param rateBps PHY rate in bits per second.
     * @param allocatedAirtimeUs Allocated airtime in microseconds.
     */
    void Add(double rateBps, double allocatedAirtimeUs);

    /** @param other Accumulator to merge. */
    void Merge(const PhyRateAccumulator& other);

    /** @return Airtime-weighted rate in megabits per second. */
    double AverageMbps() const;

    /** @return Allocated airtime in microseconds. */
    double AirtimeUs() const;
};

/**
 * Resolve an absolute timestamp to a fixed statistics window.
 *
 * @param absoluteUs Absolute timestamp in microseconds.
 * @param experimentStartUs Common trace epoch in microseconds.
 * @param maxExperimentDurationMs Maximum experiment duration in milliseconds.
 * @param windowMs Window width in milliseconds.
 * @param windowIndex Resolved zero-based window index.
 * @return True when the timestamp lies inside the experiment interval.
 */
bool GetStatisticsWindowIndex(int64_t absoluteUs,
                              int64_t experimentStartUs,
                              double maxExperimentDurationMs,
                              uint32_t windowMs,
                              uint64_t& windowIndex);

/** Own all Wi-Fi trace, aggregation, and serialization state for one scenario. */
class WifiStatistics
{
  public:
    /**
     * Construct scenario statistics collection.
     *
     * @param coordinator Traffic epoch and duration owner.
     * @param windowMs Statistics window width in milliseconds.
     */
    WifiStatistics(const TrafficCoordinator& coordinator, uint32_t windowMs);
    ~WifiStatistics();

    /**
     * Register addressing for one AP group.
     *
     * @param bssIndex Zero-based BSS index.
     * @param apAddress AP IPv4 address.
     * @param stationInterfaces Station IPv4 interfaces in index order.
     */
    void RegisterApGroup(int bssIndex,
                         Ipv4Address apAddress,
                         const Ipv4InterfaceContainer& stationInterfaces);

    /**
     * Register the stable identity of one access point.
     *
     * @param accessPointId Zero-based BSS identifier.
     * @param nodeId ns-3 node identifier.
     * @param nodeLabel Stable report label.
     * @param ipv4 Access point IPv4 address.
     */
    void RegisterAccessPointIdentity(uint32_t accessPointId,
                                     uint32_t nodeId,
                                     std::string nodeLabel,
                                     Ipv4Address ipv4);

    /**
     * Register the stable identity of one station.
     *
     * @param accessPointId Parent zero-based BSS identifier.
     * @param stationIndex Zero-based station index within the BSS.
     * @param nodeId ns-3 node identifier.
     * @param nodeLabel Stable report label.
     * @param ipv4 Station IPv4 address.
     */
    void RegisterStationIdentity(uint32_t accessPointId,
                                 uint32_t stationIndex,
                                 uint32_t nodeId,
                                 std::string nodeLabel,
                                 Ipv4Address ipv4);

    /**
     * Register one Wi-Fi device for trace collection.
     *
     * @param nodeId Owning ns-3 node identifier.
     * @param nodeLabel Stable report label.
     * @param device Wi-Fi network device.
     */
    void RegisterWifiDevice(uint32_t nodeId, std::string nodeLabel, Ptr<NetDevice> device);

    /**
     * Connect AP application traces.
     *
     * @param generator AP traffic generator.
     * @param nodeId Owning ns-3 node identifier.
     */
    void ConnectApGenerator(Ptr<APGenerator> generator, uint32_t nodeId);

    /**
     * Connect station application traces.
     *
     * @param generator Station traffic generator.
     * @param nodeId Owning ns-3 node identifier.
     */
    void ConnectStaGenerator(Ptr<StaLlmGenerator> generator, uint32_t nodeId);

    /**
     * Connect application sink traces.
     *
     * @param sink Traffic sink.
     * @param nodeId Owning ns-3 node identifier.
     * @param direction Receive direction at the local node.
     */
    void ConnectTrafficSink(Ptr<TrafficSink> sink, uint32_t nodeId, ExperimentDirection direction);

    /** Connect all Wi-Fi device transmit and receive traces exactly once. */
    void ConnectDeviceTraces();

    /** Flush current TCP congestion-window states through the experiment end. */
    void FinalizeTcpStatistics();

    /**
     * Record one parsed MAC payload sample.
     *
     * @param nowUs Absolute simulation time in microseconds.
     * @param sourceIp Source IPv4 address.
     * @param destinationIp Destination IPv4 address.
     * @param payloadBytes Application payload size in bytes.
     */
    void RecordMacPayload(int64_t nowUs,
                          const std::string& sourceIp,
                          const std::string& destinationIp,
                          uint32_t payloadBytes);

    /**
     * Build cross-layer measurements for every registered node and interval.
     *
     * @return Typed per-node cross-layer measurements.
     */
    CrossLayerSummary BuildCrossLayerSummary() const;

    /**
     * Build the transitional device transmission summary.
     *
     * @return Typed per-sender transmission measurements.
     */
    TransmissionSummary BuildTransmissionSummary();

    /**
     * Finalize raw state and build sparse windows, dense overall values, and validation.
     *
     * @return Complete typed unified experiment summary.
     */
    UnifiedExperimentSummary BuildUnifiedExperimentSummary();

    /**
     * Serialize the complete experiment output to JSON.
     *
     * @param outputPath Destination JSON path.
     * @param transmissionSummary Typed transmission measurements.
     * @param crossLayerSummary Typed cross-layer measurements.
     * @param configuration Effective scenario configuration.
     * @throws std::runtime_error if the output cannot be exclusively created or fully written.
     */
    void WriteExperimentJson(const std::string& outputPath,
                             const TransmissionSummary& transmissionSummary,
                             const CrossLayerSummary& crossLayerSummary,
                             const ScenarioConfig& configuration) const;

  private:
    friend class ::ExperimentJsonTestCase;
    friend class ::ExperimentAppTestCase;
    friend class ::ExperimentTcpTestCase;
    friend class ::ExperimentSummaryTestCase;
    friend class ::ExperimentOverallTestCase;
    friend class ::ExperimentValidationTestCase;

    /**
     * Record one congestion-window transition for a peer connection.
     *
     * @param ownerNodeId Local owner node identifier.
     * @param direction Traffic direction at the owner.
     * @param peerNodeId Remote peer node identifier.
     * @param newCwndBytes New congestion window in bytes.
     * @param absoluteTimeUs Absolute event time in microseconds.
     */
    void RecordCongestionWindow(uint32_t ownerNodeId,
                                ExperimentDirection direction,
                                uint32_t peerNodeId,
                                uint32_t newCwndBytes,
                                int64_t absoluteTimeUs);

    /**
     * Record one round-trip-time sample for a peer connection.
     *
     * @param ownerNodeId Local owner node identifier.
     * @param direction Traffic direction at the owner.
     * @param peerNodeId Remote peer node identifier.
     * @param rttUs Round-trip time in microseconds.
     * @param absoluteTimeUs Absolute event time in microseconds.
     */
    void RecordRoundTripTime(uint32_t ownerNodeId,
                             ExperimentDirection direction,
                             uint32_t peerNodeId,
                             int64_t rttUs,
                             int64_t absoluteTimeUs);

    /**
     * Adapt an AP congestion-window trace to the central recording interface.
     *
     * @param nodeId Local AP node identifier.
     * @param peer Remote station socket address.
     * @param newCwndBytes New congestion window in bytes.
     * @param eventTime Absolute event time.
     */
    void RecordApCongestionWindow(uint32_t nodeId,
                                  Address peer,
                                  uint32_t newCwndBytes,
                                  Time eventTime);

    /**
     * Adapt an AP round-trip-time trace to the central recording interface.
     *
     * @param nodeId Local AP node identifier.
     * @param peer Remote station socket address.
     * @param rtt Round-trip time sample.
     * @param eventTime Absolute event time.
     */
    void RecordApRoundTripTime(uint32_t nodeId, Address peer, Time rtt, Time eventTime);

    /**
     * Adapt a station congestion-window trace to the central recording interface.
     *
     * @param nodeId Local station node identifier.
     * @param peer Remote AP socket address.
     * @param newCwndBytes New congestion window in bytes.
     * @param eventTime Absolute event time.
     */
    void RecordStaCongestionWindow(uint32_t nodeId,
                                   Address peer,
                                   uint32_t newCwndBytes,
                                   Time eventTime);

    /**
     * Adapt a station round-trip-time trace to the central recording interface.
     *
     * @param nodeId Local station node identifier.
     * @param peer Remote AP socket address.
     * @param rtt Round-trip time sample.
     * @param eventTime Absolute event time.
     */
    void RecordStaRoundTripTime(uint32_t nodeId, Address peer, Time rtt, Time eventTime);

    /**
     * Record one socket-accepted application send.
     *
     * @param nodeId Local sending node identifier.
     * @param direction Traffic direction.
     * @param peerNodeId Remote peer node identifier when resolved.
     * @param agentKey Application-level agent identifier.
     * @param acceptedBytes Payload bytes accepted by the socket.
     * @param absoluteTimeUs Absolute send time in microseconds.
     */
    void RecordAcceptedApplicationSend(uint32_t nodeId,
                                       ExperimentDirection direction,
                                       std::optional<uint32_t> peerNodeId,
                                       const std::string& agentKey,
                                       uint32_t acceptedBytes,
                                       int64_t absoluteTimeUs);

    /**
     * Record one application send rejection.
     *
     * @param nodeId Local sending node identifier.
     * @param direction Traffic direction.
     * @param peerNodeId Remote peer node identifier when resolved.
     * @param agentKey Application-level agent identifier.
     * @param droppedBytes Payload bytes rejected by the socket.
     * @param absoluteTimeUs Absolute drop time in microseconds.
     */
    void RecordApplicationDrop(uint32_t nodeId,
                               ExperimentDirection direction,
                               std::optional<uint32_t> peerNodeId,
                               const std::string& agentKey,
                               uint32_t droppedBytes,
                               int64_t absoluteTimeUs);

    /**
     * Record one application sink receive.
     *
     * @param nodeId Local receiving node identifier.
     * @param direction Traffic direction.
     * @param peerNodeId Remote peer node identifier when resolved.
     * @param receivedBytes Payload bytes received by the sink.
     * @param absoluteTimeUs Absolute receive time in microseconds.
     */
    void RecordApplicationReceive(uint32_t nodeId,
                                  ExperimentDirection direction,
                                  std::optional<uint32_t> peerNodeId,
                                  uint32_t receivedBytes,
                                  int64_t absoluteTimeUs);

    /** Adapt an AP accepted-send trace to the central recording interface. */
    void RecordApAcceptedApplicationSend(uint32_t nodeId,
                                         Address station,
                                         std::string agentKey,
                                         uint32_t acceptedBytes,
                                         Time transmitTime);

    /** Adapt an AP drop trace to the central recording interface. */
    void RecordApApplicationDrop(uint32_t nodeId,
                                 Address station,
                                 std::string agentKey,
                                 uint32_t droppedBytes,
                                 Time transmitTime);

    /** Adapt a station accepted-send trace to the central recording interface. */
    void RecordStaAcceptedApplicationSend(uint32_t nodeId,
                                          std::optional<uint32_t> peerNodeId,
                                          std::string agentKey,
                                          uint32_t acceptedBytes,
                                          Time transmitTime);

    /** Adapt a station drop trace to the central recording interface. */
    void RecordStaApplicationDrop(uint32_t nodeId,
                                  std::optional<uint32_t> peerNodeId,
                                  std::string agentKey,
                                  uint32_t droppedBytes,
                                  Time transmitTime);

    /** Adapt a sink receive trace to the central recording interface. */
    void RecordTrafficSinkReceive(uint32_t nodeId,
                                  ExperimentDirection direction,
                                  uint64_t receivedBytes,
                                  Address remoteAddress);

    std::unique_ptr<WifiStatisticsState> m_state; ///< Scenario statistics state.
};

} // namespace ns3

#endif // WIFI_STATISTICS_H
