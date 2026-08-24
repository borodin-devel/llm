#ifndef STA_LLM_GENERATOR_H
#define STA_LLM_GENERATOR_H

#include "traffic-schedule.h"

#include "ns3/callback.h"
#include "ns3/data-rate.h"
#include "ns3/event-id.h"
#include "ns3/source-application.h"
#include "ns3/traced-callback.h"
#include "ns3/traced-value.h"

#include <cstdint>
#include <map>
#include <vector>

namespace ns3
{

class Socket;
class Packet;
class Ipv4Address;

/**
 * @ingroup applications
 *
 * TCP station generator that sends agent payloads at scheduled offsets.
 *
 * Merges legacy operations into one uplink schedule and starts payload traffic
 * only after the common experiment epoch is available.
 */
class StaLlmGenerator : public SourceApplication
{
  public:
    /**
     * Get the registered type identifier.
     *
     * @return Application TypeId.
     */
    static TypeId GetTypeId();

    StaLlmGenerator();
    ~StaLlmGenerator() override;

    /**
     * Accept the legacy numeric agent list.
     *
     * @param agentIds Numeric agent identifiers.
     */
    void SetAgentIds(std::vector<int64_t> agentIds);

    /**
     * Set legacy traffic operations.
     *
     * @param operationsByAgent Traffic operations grouped by agent.
     */
    void SetAgentMap(
        std::map<std::string, std::vector<std::tuple<int, double, double, int>>> operationsByAgent);

    /**
     * Set the callback fired once the STA TCP connection is ready.
     *
     * @param callback Readiness callback.
     */
    void SetReadyCallback(Callback<void> callback);

    /**
     * Schedule payloads against the common experiment epoch.
     *
     * @param experimentStartMs Common experiment epoch in milliseconds.
     */
    void StartTraffic(uint64_t experimentStartMs);

  private:
    void DoDispose() override;
    void DoStartApplication() override;
    void DoStopApplication() override;
    void CancelEvents() override;

    void DoConnectionSucceeded(Ptr<Socket> socket) override;
    void DoConnectionFailed(Ptr<Socket> socket) override;

    /** Schedule every uplink payload against the common epoch. */
    void ScheduleAllTransmissions();

    /**
     * Send one uplink payload.
     *
     * @param agentKey Application-level agent identifier.
     * @param payloadBytes Requested payload size in bytes.
     * @param traceTimeMs Trace-relative send time in milliseconds.
     */
    void SendAgentData(std::string agentKey, uint32_t payloadBytes, double traceTimeMs);

    /**
     * Record bytes accepted by the station socket.
     *
     * @param agentKey Application-level agent identifier.
     * @param acceptedBytes Bytes accepted by the socket.
     * @param transmitTime Application transmit time.
     */
    void RecordAcceptedSend(const std::string& agentKey, uint32_t acceptedBytes, Time transmitTime);

    /**
     * Consume received TCP data.
     *
     * @param socket Socket with available data.
     */
    void HandleRead(Ptr<Socket> socket);

    /**
     * Record a congestion-window update.
     *
     * @param oldCwnd Previous congestion window in bytes.
     * @param newCwnd New congestion window in bytes.
     */
    void OnCwndChange(uint32_t oldCwnd, uint32_t newCwnd);

    /**
     * Record a round-trip-time sample.
     *
     * @param oldRtt Previous RTT sample.
     * @param lastRtt New RTT sample.
     */
    void OnLastRttChange(Time oldRtt, Time lastRtt);

    /** Print final per-second metrics. */
    void PrintPerSecondMetrics();

    /**
     * Report agents that never reached scheduling.
     *
     * @param localAddress Local station address shown in the diagnostic.
     */
    void ReportUnscheduledAgents(Ipv4Address localAddress) const;

    LegacyAgentOperations m_operationsByAgent;      ///< Legacy operation input by agent.
    std::vector<ScheduledPayload> m_uplinkSchedule; ///< Ordered uplink payloads.

    EventId m_sendEvent;                  ///< Most recently scheduled send event.
    bool m_transmissionsScheduled{false}; ///< Whether payload scheduling started.
    uint64_t m_experimentStartMs{0};      ///< Simulation time corresponding to trace time zero.
    Callback<void> m_readyCallback;       ///< Common-barrier readiness callback.
    bool m_readyReported{false};          ///< Whether the readiness callback fired.
    std::vector<std::string> m_unscheduledAgentKeys; ///< Agent keys not yet scheduled.

    /** Metrics for one absolute simulation-second bucket. */
    struct PerSecondStats
    {
        uint64_t totalBytes{0};                     ///< Total accepted payload bytes.
        std::map<std::string, uint64_t> agentBytes; ///< Accepted bytes by agent.
        double lastCwnd{0.0};                       ///< Last congestion window in bytes.
        uint64_t rttSamples{0};                     ///< Number of nonzero RTT samples.
        double rttSumUs{0.0};                       ///< Sum of RTT samples in microseconds.
    };

    std::map<uint32_t, PerSecondStats> m_metricsByAbsoluteSecond; ///< Metrics by absolute second.
    double m_currentCwnd{0.0}; ///< Most recently observed congestion window in bytes.
    TracedCallback<std::string, uint32_t, Time> m_txTraceCustom;  ///< Accepted sends.
    TracedCallback<std::string, uint32_t, Time> m_appTxDropTrace; ///< Rejected bytes.

    /** Complete application-send trace callback signature. */
    using AgentSendCallback = void (*)(std::string agentKey,
                                       uint32_t bytes,
                                       Time startTime,
                                       Time endTime);
    TracedCallback<std::string, uint32_t, Time, Time> m_agentSendTrace; ///< Sends.
};

} // namespace ns3

#endif // STA_LLM_GENERATOR_H
