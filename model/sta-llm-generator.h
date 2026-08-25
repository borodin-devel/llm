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
     * Consume received TCP data.
     *
     * @param socket Socket with available data.
     */
    void HandleRead(Ptr<Socket> socket);

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
