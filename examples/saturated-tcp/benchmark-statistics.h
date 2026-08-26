#ifndef SATURATED_TCP_BENCHMARK_STATISTICS_H
#define SATURATED_TCP_BENCHMARK_STATISTICS_H

#include "../statistics/output-types.h"
#include "../statistics/types.h"
#include "sta-phy-metrics.h"

#include "ns3/callback.h"
#include "ns3/nstime.h"
#include "ns3/ptr.h"

#include <cstdint>
#include <functional>
#include <iosfwd>
#include <map>
#include <memory>
#include <string>
#include <vector>

class SaturatedTcpBenchmarkSummaryTestCase;
class SaturatedTcpBenchmarkLifecycleTestCase;

namespace ns3
{

class AccessTrackingStaWifiMac;
class AccessWaitTracker;
class JsonWriter;
class QosTxop;
class WifiNetDevice;
class WifiPhy;
struct SaturatedTcpConfig;

/** Own station-only saturated TCP benchmark measurement and aggregation state. */
class SaturatedTcpStatistics
{
  public:
    /**
     * Construct benchmark statistics for a one-second measurement epoch.
     *
     * @param windowMs Fixed window width in milliseconds.
     * @throws std::invalid_argument if the window width is zero or does not divide one second.
     */
    explicit SaturatedTcpStatistics(uint32_t windowMs);

    /** Destroy benchmark statistics state. */
    ~SaturatedTcpStatistics();

    /**
     * Register one access point identity.
     *
     * @param accessPointId Zero-based BSS identifier.
     * @param nodeId ns-3 node identifier.
     * @param nodeLabel Stable report label.
     * @param ipv4 Access point IPv4 address.
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
     * @param nodeLabel Stable report label.
     * @param ipv4 Station IPv4 address.
     */
    void RegisterStation(uint32_t accessPointId,
                         uint32_t stationIndex,
                         uint32_t nodeId,
                         std::string nodeLabel,
                         std::string ipv4);

    /**
     * Connect one registered station device's MAC, TXOP, and PHY traces.
     *
     * Access point devices and devices without the benchmark station MAC are rejected. Every PHY
     * owned by the station device is connected; no access point trace is ever connected.
     *
     * @param device Registered station Wi-Fi device.
     * @throws std::invalid_argument if the device is null, unregistered, not a station, lacks the
     *         required MAC/PHY/TXOP objects, or was already connected.
     */
    void ConnectStation(Ptr<WifiNetDevice> device);

    /**
     * Open the exact one-second benchmark measurement epoch.
     *
     * Trace callbacks delivered before this call do not contribute.
     *
     * @param experimentStartNs Inclusive experiment start in nanoseconds.
     * @throws std::logic_error if measurement has already started.
     * @throws std::invalid_argument if not every registered station is connected.
     * @throws std::overflow_error if the one-second endpoint exceeds the nanosecond range.
     */
    void Start(int64_t experimentStartNs);

    /**
     * Close pending access waits and finalize station raw state.
     *
     * Repeated calls after successful finalization have no effect.
     *
     * @param experimentEndNs Exclusive experiment end in nanoseconds.
     * @throws std::logic_error if measurement has not started.
     * @throws std::invalid_argument if the endpoint is not exactly one second after the start.
     */
    void Finalize(int64_t experimentEndNs);

    /**
     * Build sparse windows, dense overall values, and the fixed validation shape.
     *
     * @return Complete shared-schema benchmark summary.
     * @throws std::logic_error if measurement is not finalized.
     * @throws std::invalid_argument if raw or required overall station metrics are invalid.
     */
    UnifiedExperimentSummary BuildSummary() const;

  private:
    friend class ::SaturatedTcpBenchmarkSummaryTestCase;
    friend class ::SaturatedTcpBenchmarkLifecycleTestCase;

    /**
     * Build the public summary from literal per-station raw windows.
     *
     * @param rawWindows Raw chronological windows keyed by station node identifier.
     * @return Complete shared-schema benchmark summary.
     */
    UnifiedExperimentSummary BuildSummaryFromRaw(
        const std::map<uint32_t, std::vector<StationPhyMetricAccumulator>>& rawWindows) const;

    /**
     * Build the public summary from configured windows and independent overall raw state.
     *
     * @param rawWindows Raw configured windows keyed by station node identifier.
     * @param rawOverall Independent one-second raw values keyed by station node identifier.
     * @return Complete shared-schema benchmark summary.
     */
    UnifiedExperimentSummary BuildSummaryFromRaw(
        const std::map<uint32_t, std::vector<StationPhyMetricAccumulator>>& rawWindows,
        const std::map<uint32_t, StationPhyMetricAccumulator>& rawOverall) const;

    /** One exact QoS TXOP trace subscription. */
    struct TxopTraceConnection
    {
        Ptr<QosTxop> source;                          ///< Subscribed QoS TXOP.
        Callback<void, Time, Time, uint8_t> callback; ///< Exact connected callback.
        bool connected{false};                        ///< Whether the callback was connected.
    };

    /** One exact station PHY trace subscription. */
    struct PhyTraceConnection
    {
        Ptr<WifiPhy> source; ///< Subscribed station PHY.
        Callback<void, WifiConstPsduMap, WifiTxVector, double>
            callback;          ///< Exact connected callback.
        bool connected{false}; ///< Whether the callback was connected.
    };

    /** Every trace subscription owned for one connected station. */
    struct StationTraceConnections
    {
        Ptr<AccessTrackingStaWifiMac> mac; ///< Subscribed benchmark station MAC.
        Callback<void, uint8_t, uint8_t>
            accessRequestedCallback;                 ///< Exact access-request callback.
        bool accessRequestedConnected{false};        ///< Whether the MAC callback was connected.
        std::vector<TxopTraceConnection> txopTraces; ///< Exact per-AC TXOP subscriptions.
        std::vector<PhyTraceConnection> phyTraces;   ///< Exact per-PHY subscriptions.
    };

    /** Disconnect locally owned subscriptions unless durable ownership is transferred. */
    class StationTraceConnectionGuard
    {
      public:
        /**
         * Guard one local station subscription record.
         *
         * @param connections Local connection record.
         */
        explicit StationTraceConnectionGuard(StationTraceConnections& connections);

        /** Disconnect every locally owned connected callback unless disarmed. */
        ~StationTraceConnectionGuard();

        /** Transfer responsibility to durable owner state. */
        void Disarm() noexcept;

      private:
        StationTraceConnections& m_connections; ///< Locally owned subscription record.
        bool m_armed{true};                     ///< Whether destruction must disconnect.
    };

    /**
     * Disconnect every recorded callback for one station.
     *
     * @param connections Exact station trace connections to remove.
     */
    static void DisconnectTraceConnections(StationTraceConnections& connections) noexcept;

    /** Disconnect every station trace before measurement state is disposed. */
    void DisconnectAllTraceConnections() noexcept;

    /**
     * Record a station channel-access request callback.
     *
     * @param stationNodeId Bound station node identifier.
     * @param ac Access category.
     * @param linkId Link identifier.
     */
    void NotifyAccessRequested(uint32_t stationNodeId, uint8_t ac, uint8_t linkId);

    /**
     * Record a station historical TXOP grant callback.
     *
     * @param stationNodeId Bound station node identifier.
     * @param ac Bound access category.
     * @param start Historical TXOP start.
     * @param duration Historical TXOP duration.
     * @param linkId Link identifier.
     */
    void NotifyTxopGranted(uint32_t stationNodeId,
                           uint8_t ac,
                           Time start,
                           Time duration,
                           uint8_t linkId);

    /**
     * Record one station PHY transmission callback.
     *
     * @param stationNodeId Bound station node identifier.
     * @param band Bound station PHY band.
     * @param psduMap Transmitted PSDUs.
     * @param txVector Actual transmission vector.
     * @param txPowerW Transmission power in watts.
     */
    void NotifyPhyTxPsduBegin(uint32_t stationNodeId,
                              WifiPhyBand band,
                              WifiConstPsduMap psduMap,
                              WifiTxVector txVector,
                              double txPowerW);

    uint32_t m_windowMs;                 ///< Fixed statistics window width in milliseconds.
    int64_t m_windowNs;                  ///< Fixed statistics window width in nanoseconds.
    ExperimentEntityRegistry m_registry; ///< Deterministic AP and station identity registry.
    std::map<uint32_t, Ptr<WifiNetDevice>> m_stationDevices; ///< Connected STA devices by node ID.
    std::map<uint32_t, StationTraceConnections>
        m_traceConnections; ///< Exact subscriptions by station node ID.
    std::function<void()>
        m_subscriptionOwnershipHook; ///< Injected post-connection ownership-transfer seam.
    std::unique_ptr<StationPhyMetricRecorder> m_phyRecorder; ///< Task 5 raw PPDU recorder.
    std::unique_ptr<StationPhyMetricRecorder>
        m_overallPhyRecorder; ///< Independent one-window Task 5 overall recorder.
    std::map<uint32_t, std::unique_ptr<AccessWaitTracker>>
        m_accessWaitTrackers;       ///< Task 4 wait trackers by station node ID.
    int64_t m_experimentStartNs{0}; ///< Inclusive measurement start in nanoseconds.
    int64_t m_experimentEndNs{0};   ///< Exclusive one-second endpoint in nanoseconds.
    bool m_started{false};          ///< Whether the measurement epoch has opened.
    bool m_finalized{false};        ///< Whether pending state has been finalized.
};

/**
 * Write saturated benchmark measurement semantics.
 *
 * @param writer Structured JSON writer.
 */
void WriteSaturatedMeasurementSemantics(JsonWriter& writer);

/**
 * Stream a validated saturated benchmark through the shared JSON hierarchy.
 *
 * @param output Destination stream.
 * @param summary Finalized benchmark summary.
 * @param config Effective benchmark configuration.
 * @throws std::invalid_argument if the summary or configuration is inconsistent.
 */
void WriteSaturatedTcpExperimentJson(std::ostream& output,
                                     const UnifiedExperimentSummary& summary,
                                     const SaturatedTcpConfig& config);

/**
 * Exclusively create and write a validated saturated benchmark JSON file.
 *
 * @param outputPath Destination JSON path.
 * @param summary Finalized benchmark summary.
 * @param config Effective benchmark configuration.
 * @throws std::invalid_argument if the summary or configuration is inconsistent.
 * @throws std::runtime_error if the output cannot be exclusively created or fully written.
 */
void WriteSaturatedTcpExperimentJson(const std::string& outputPath,
                                     const UnifiedExperimentSummary& summary,
                                     const SaturatedTcpConfig& config);

/** Internal benchmark output seams shared with deterministic lifecycle tests. */
namespace saturated_tcp_internal
{

/** Callback that writes one complete JSON body to an owned output stream. */
using JsonBodyWriter = std::function<void(std::ostream&)>;

/**
 * Exclusively create a file, invoke its body writer, and remove partial owned output on failure.
 *
 * @param outputPath Destination path.
 * @param writeBody Complete body writer.
 * @throws std::invalid_argument if @p writeBody is empty.
 * @throws std::runtime_error if exclusive creation, writing, flushing, closing, or cleanup fails.
 */
void WriteExclusiveJsonFile(const std::string& outputPath, const JsonBodyWriter& writeBody);

} // namespace saturated_tcp_internal

} // namespace ns3

#endif // SATURATED_TCP_BENCHMARK_STATISTICS_H
