#ifndef EXPERIMENT_STATISTICS_H
#define EXPERIMENT_STATISTICS_H

#include "experiment-statistics-types.h"
#include "experiment-window-output.h"

#include "ns3/address.h"
#include "ns3/ipv4-address.h"
#include "ns3/ptr.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

class ExperimentAppTestCase;
class ExperimentTcpTestCase;
class ExperimentSummaryTestCase;
class ExperimentOverallTestCase;
class ExperimentValidationTestCase;

namespace ns3
{

class APGenerator;
class NetDevice;
class StaLlmGenerator;
class Time;
class TrafficCoordinator;
class TrafficSink;
struct ScenarioConfig;
struct ExperimentStatisticsState;

/** Own all experiment trace, aggregation, finalization, and serialization state. */
class ExperimentStatistics
{
  public:
    /**
     * Construct scenario statistics collection.
     *
     * @param coordinator Traffic epoch and duration owner.
     * @param windowMs Statistics window width in milliseconds.
     */
    ExperimentStatistics(const TrafficCoordinator& coordinator, uint32_t windowMs);
    ~ExperimentStatistics();

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
     * @param device Wi-Fi network device.
     */
    void RegisterWifiDevice(uint32_t nodeId, Ptr<NetDevice> device);

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

    /** Finalize device matching and per-peer TCP state exactly once. */
    void Finalize();

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
     * @param configuration Effective scenario configuration.
     * @throws std::runtime_error if the output cannot be exclusively created or fully written.
     */
    void WriteExperimentJson(const std::string& outputPath, const ScenarioConfig& configuration);

  private:
    friend class ::ExperimentAppTestCase;
    friend class ::ExperimentTcpTestCase;
    friend class ::ExperimentSummaryTestCase;
    friend class ::ExperimentOverallTestCase;
    friend class ::ExperimentValidationTestCase;

    /** Flush current TCP congestion-window states through the experiment end. */
    void FinalizeTcpStatistics();

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

    /**
     * Adapt an AP accepted-send trace to the central recording interface.
     *
     * @param nodeId Local AP node identifier.
     * @param station Remote station socket address.
     * @param agentKey Application-level agent identifier.
     * @param acceptedBytes Payload bytes accepted by the socket.
     * @param transmitTime Absolute send time.
     */
    void RecordApAcceptedApplicationSend(uint32_t nodeId,
                                         Address station,
                                         std::string agentKey,
                                         uint32_t acceptedBytes,
                                         Time transmitTime);

    /**
     * Adapt an AP drop trace to the central recording interface.
     *
     * @param nodeId Local AP node identifier.
     * @param station Remote station socket address.
     * @param agentKey Application-level agent identifier.
     * @param droppedBytes Payload bytes rejected by the socket.
     * @param transmitTime Absolute drop time.
     */
    void RecordApApplicationDrop(uint32_t nodeId,
                                 Address station,
                                 std::string agentKey,
                                 uint32_t droppedBytes,
                                 Time transmitTime);

    /**
     * Adapt a station accepted-send trace to the central recording interface.
     *
     * @param nodeId Local station node identifier.
     * @param peerNodeId Remote AP node identifier when resolved.
     * @param agentKey Application-level agent identifier.
     * @param acceptedBytes Payload bytes accepted by the socket.
     * @param transmitTime Absolute send time.
     */
    void RecordStaAcceptedApplicationSend(uint32_t nodeId,
                                          std::optional<uint32_t> peerNodeId,
                                          std::string agentKey,
                                          uint32_t acceptedBytes,
                                          Time transmitTime);

    /**
     * Adapt a station drop trace to the central recording interface.
     *
     * @param nodeId Local station node identifier.
     * @param peerNodeId Remote AP node identifier when resolved.
     * @param agentKey Application-level agent identifier.
     * @param droppedBytes Payload bytes rejected by the socket.
     * @param transmitTime Absolute drop time.
     */
    void RecordStaApplicationDrop(uint32_t nodeId,
                                  std::optional<uint32_t> peerNodeId,
                                  std::string agentKey,
                                  uint32_t droppedBytes,
                                  Time transmitTime);

    /**
     * Adapt a sink receive trace to the central recording interface.
     *
     * @param nodeId Local receiving node identifier.
     * @param direction Traffic direction at the local node.
     * @param receivedBytes Received application payload bytes.
     * @param remoteAddress Remote peer socket address.
     */
    void RecordTrafficSinkReceive(uint32_t nodeId,
                                  ExperimentDirection direction,
                                  uint64_t receivedBytes,
                                  Address remoteAddress);

    std::unique_ptr<ExperimentStatisticsState> m_state; ///< Scenario statistics state.
};

} // namespace ns3

#endif // EXPERIMENT_STATISTICS_H
