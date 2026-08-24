// model/sta-llm-generator.h
//
// Traffic Generator Application - TCP sender for station
// Based on ns3::OnOffApplication pattern (inherits SourceApplication)
//

#ifndef STA_LLM_GENERATOR_H
#define STA_LLM_GENERATOR_H

#include "traffic-schedule.h"

#include "ns3/source-application.h"
#include "ns3/event-id.h"
#include "ns3/data-rate.h"
#include "ns3/traced-callback.h"
#include "ns3/traced-value.h"
#include "ns3/callback.h"

#include <map>
#include <vector>
#include <cstdint>

namespace ns3
{

class Socket;
class Packet;

/**
 * @ingroup applications
 *
 * @brief TCP traffic generator that sends agent payloads at scheduled offsets.
 *
 * Takes a map of agentId -> list of operations, merges all operations into
 * a single sorted vector by startOffsetMs, and schedules TCP transmissions
 * at those relative times. Each operation sends uplinkBytes.
 *
 * Metrics tracked:
 *   - uplink throughput: bytes/second per second of simulation
 *   - TCP congestion window (Cwnd) per-second
 *   - per-agent bandwidth share: % of total throughput per second
 *   - channel utilization estimate
 */
class StaLlmGenerator : public SourceApplication
{
  public:
    /**
     * @brief Get the type ID.
     * @return the object TypeId
     */
    static TypeId GetTypeId();

    StaLlmGenerator();
    ~StaLlmGenerator() override;

    /**
     * @brief Set the list of agent IDs to process.
     */
    void SetAgentIds(std::vector<int64_t> agentIds);

    /**
     * @brief Set the agent operations map (agentId -> operations).
     */
    void SetAgentMap(
      std::map<std::string, std::vector<std::tuple<int, double, double, int>>> agentsMap);

    /**
     * @brief Set callback fired once the STA TCP connection is ready.
     */
    void SetReadyCallback(Callback<void> callback);

    /**
     * @brief Schedule payload against the common absolute experiment epoch.
     */
    void StartTraffic(uint64_t experimentStartMs);

  private:
    void DoDispose() override;
    void DoStartApplication() override;
    void DoStopApplication() override;
    void CancelEvents() override;

    void DoConnectionSucceeded(Ptr<Socket> socket) override;
    void DoConnectionFailed(Ptr<Socket> socket) override;

    void ScheduleAllTransmissions();
    void SendAgentData(std::string agentKey, uint32_t bytes, double scheduledMs);
    void HandleRead(Ptr<Socket> socket);
    void OnCwndChange(uint32_t, uint32_t newCwnd);
    void OnLastRttChange(Time, Time lastRtt);
    void PrintPerSecondMetrics();

    LegacyAgentOperations m_operationsByAgent; ///< Legacy operation input by agent.
    std::vector<ScheduledPayload> m_uplinkSchedule; ///< Ordered uplink payloads.

    // Event for next send
    EventId m_sendEvent;

    // Prevent repeated scheduling if the connection callback fires again.
    bool m_transmissionsScheduled{false};

    // Absolute simulation time corresponding to trace t=0.
    uint64_t m_experimentStartMs{0};

    Callback<void> m_readyCallback;
    bool m_readyReported{false};

    // All agent keys (set before scheduling, drained during it)
    std::vector<std::string> m_allAgentKeys;

    struct PerSecondStats
    {
        uint64_t totalBytes{0};

        std::map<std::string, uint64_t> agentBytes;

        double lastCwnd{0.0};
        uint64_t rttSamples{0};
        double rttSumUs{0.0};
    };

    // Second number -> metrics collected during this second.
    //
    // For example, key 0 contains events from [0, 1) seconds,
    // key 1 contains events from [1, 2) seconds, etc.
    std::map<uint32_t, PerSecondStats> m_metricsByAbsoluteSecond;

    // Metrics: current Cwnd
    double m_currentCwnd{0.0};

    // Traced callbacks
    TracedCallback<std::string, uint32_t, Time> m_txTraceCustom;
    TracedCallback<std::string, uint32_t, Time> m_appTxDropTrace;

    typedef void (*AgentSendCallback)(std::string agentKey,
                                      uint32_t bytes,
                                      Time startTime,
                                      Time endTime);
    TracedCallback<std::string, uint32_t, Time, Time> m_agentSendTrace;
};

} // namespace ns3

#endif /* STA_LLM_GENERATOR_H */
