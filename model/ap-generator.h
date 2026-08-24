// model/ap-generator.h
//
// AP Generator Application - Downlink sender from Access Point
// Sends downlink data to stations based on agent-to-station mapping
//

#ifndef AP_GENERATOR_H
#define AP_GENERATOR_H

#include "traffic-schedule.h"

#include "ns3/application.h"
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
class TcpSocketBase;

/**
 * @ingroup applications
 *
 * @brief Downlink traffic generator at the Access Point.
 *
 * Takes a map of agentId -> operations and an agent-to-station map.
 * Aggregates operations per station, sorts by startMs, and schedules
 * TCP downlink transmissions to each station.
 *
 * Each operation tuple: (downlinkBytes, endMs, startMs, uplinkBytes)
 *   - downlinkBytes: bytes to send to the station
 *   - endMs: transmission duration estimate
 *   - startMs: when to start sending (absolute, relative to sim start)
 *   - uplinkBytes: reserved for future use (not sent by AP)
 */
class APGenerator : public Application
{
  public:
    /**
     * @brief Get the type ID.
     * @return the object TypeId
     */
    static TypeId GetTypeId();

    APGenerator();
    ~APGenerator() override;

    /**
     * @brief Set the agent-to-station mapping.
     * @param mapping agentId -> stationAddress
     */
    void SetAgentStationMap(std::map<std::string, Address> mapping);

    /**
     * @brief Set the agent operations map.
     * @param agentsMap agentId -> list of operations
     */
    void SetAgentMap(
      std::map<std::string, std::vector<std::tuple<int, double, double, int>>> agentsMap);

    /**
     * @brief Set callback fired once all AP-owned TCP connections are ready.
     */
    void SetReadyCallback(Callback<void> callback);

    /**
     * @brief Schedule payload against the common absolute experiment epoch.
     */
    void StartTraffic(uint64_t experimentStartMs);

  private:
    void DoDispose() override;
    void StartApplication() override;
    void StopApplication() override;

    /**
     * @brief Aggregate operations per station and sort.
     */
    void AggregateAndSortOperations();

    /**
     * @brief Create sockets and connect to all stations.
     */
    void ConnectToStations();

    /**
     * @brief Schedule transmissions for a specific station.
     */
    void ScheduleStationTransmissions(const Address &station);

    /**
     * @brief Send downlink data to a station.
     */
    void SendDownlink(const Address &station,
                      const std::string &agentKey,
                      uint32_t bytes,
                      double startMs);

    /**
     * @brief Handle received data (ACKs).
     */
    void HandleRead(Ptr<Socket> socket);

    /**
     * @brief Connection succeeded callback.
     */
    void OnConnectionSucceeded(Ptr<Socket> socket);

    /**
     * @brief Connection failed callback.
     */
    void OnConnectionFailed(Ptr<Socket> socket);

    void ReportReadyIfComplete();

    /**
     * @brief Cwnd change callback.
     */
    void OnCwndChange(uint32_t, uint32_t newCwnd);

    /**
     * @brief Print per-second metrics at the end.
     */
    void PrintPerSecondMetrics();

    std::map<std::string, Address> m_stationAddressByAgent; ///< Destination station by agent.
    LegacyAgentOperations m_operationsByAgent;             ///< Legacy operation input by agent.
    DownlinkSchedulesByStation m_downlinkSchedulesByStation; ///< Ordered payloads by station.

    // Per-station sockets
    std::map<Address, Ptr<Socket>> m_socketByStation;
    // Track which sockets have connected
    std::map<Address, bool> m_isConnectedByStation;
    // Reverse mapping: socket pointer -> station address (for callbacks)
    std::map<const void*, Address> m_socketToStation;

    // Per-station pending events
    std::map<Address, EventId> m_sendEventByStation;

    // Per-station metrics
    struct StationMetrics
    {
        double currentCwnd{0}; ///< Most recently observed congestion window.
    };
    std::map<Address, StationMetrics> m_stationMetrics;

    // Absolute simulation time corresponding to trace t=0.
    uint64_t m_experimentStartMs{0};

    bool m_readyReported{false};
    bool m_trafficStarted{false};
    Callback<void> m_readyCallback;

    // All agent keys (set before scheduling, drained during it)
    std::vector<std::string> m_allAgentKeys;

    struct PerSecondStats
    {
        uint64_t totalBytes{0};

        std::map<std::string, uint64_t> agentBytes;
        std::map<Address, uint64_t> stationBytes;

        double lastCwnd{0.0};
    };

    // Second number -> metrics collected during this second.
    std::map<uint32_t, PerSecondStats> m_metricsByAbsoluteSecond;

    // Traced callbacks
    TracedCallback<Address, std::string, uint32_t, Time> m_txTrace;
    TracedCallback<Address, std::string, uint32_t, Time> m_appTxDropTrace;

    typedef void (*AgentSendCallback)(Address station,
                                      std::string agentKey,
                                      uint32_t bytes,
                                      Time startTime,
                                      Time endTime);
    TracedCallback<Address, std::string, uint32_t, Time, Time> m_agentSendTrace;
};

} // namespace ns3

#endif /* AP_GENERATOR_H */
