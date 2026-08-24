#ifndef AP_GENERATOR_H
#define AP_GENERATOR_H

#include "traffic-schedule.h"

#include "ns3/application.h"
#include "ns3/callback.h"
#include "ns3/data-rate.h"
#include "ns3/event-id.h"
#include "ns3/traced-callback.h"
#include "ns3/traced-value.h"

#include <cstdint>
#include <map>
#include <vector>

namespace ns3
{

class Socket;
class Packet;
class TcpSocketBase;
class Ipv4Address;

/**
 * @ingroup applications
 *
 * Downlink traffic generator at the access point.
 *
 * Groups legacy operations by destination station and schedules TCP payloads
 * against a common experiment epoch.
 */
class APGenerator : public Application
{
  public:
    /**
     * Get the registered type identifier.
     *
     * @return Application TypeId.
     */
    static TypeId GetTypeId();

    APGenerator();
    ~APGenerator() override;

    /**
     * Set the destination station for each agent.
     *
     * @param stationAddressByAgent Destination station address by agent.
     */
    void SetAgentStationMap(std::map<std::string, Address> stationAddressByAgent);

    /**
     * Set legacy traffic operations.
     *
     * @param operationsByAgent Traffic operations grouped by agent.
     */
    void SetAgentMap(
        std::map<std::string, std::vector<std::tuple<int, double, double, int>>> operationsByAgent);

    /**
     * Set the callback fired once all AP-owned TCP connections are ready.
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
    void StartApplication() override;
    void StopApplication() override;

    /** Build ordered downlink schedules by destination station. */
    void BuildSchedules();

    /** Create sockets and connect to all destination stations. */
    void ConnectToStations();

    /**
     * Configure and connect one station socket.
     *
     * @param stationAddress Destination station address.
     * @param socket Newly created TCP socket.
     */
    void ConfigureSocket(const Address& stationAddress, Ptr<Socket> socket);

    /**
     * Schedule transmissions for one station.
     *
     * @param stationAddress Destination station address.
     */
    void ScheduleStationTransmissions(const Address& stationAddress);

    /**
     * Send one downlink payload.
     *
     * @param stationAddress Destination station address.
     * @param agentKey Application-level agent identifier.
     * @param payloadBytes Requested payload size in bytes.
     * @param traceTimeMs Trace-relative send time in milliseconds.
     */
    void SendDownlink(const Address& stationAddress,
                      const std::string& agentKey,
                      uint32_t payloadBytes,
                      double traceTimeMs);

    /**
     * Record payload bytes accepted by a station socket.
     *
     * @param stationAddress Destination station address.
     * @param agentKey Application-level agent identifier.
     * @param acceptedBytes Bytes accepted by the socket.
     * @param transmitTime Application transmit time.
     */
    void RecordAcceptedSend(const Address& stationAddress,
                            const std::string& agentKey,
                            uint32_t acceptedBytes,
                            Time transmitTime);

    /**
     * Consume received TCP data.
     *
     * @param socket Socket with available data.
     */
    void HandleRead(Ptr<Socket> socket);

    /**
     * Handle successful TCP connection establishment.
     *
     * @param socket Connected socket.
     */
    void OnConnectionSucceeded(Ptr<Socket> socket);

    /**
     * Handle failed TCP connection establishment.
     *
     * @param socket Socket that failed to connect.
     */
    void OnConnectionFailed(Ptr<Socket> socket);

    /** Report readiness after every station connection succeeds. */
    void ReportReadyIfComplete();

    /**
     * Record a congestion-window update.
     *
     * @param oldCwnd Previous congestion window in bytes.
     * @param newCwnd New congestion window in bytes.
     */
    void OnCwndChange(uint32_t oldCwnd, uint32_t newCwnd);

    /** Print final per-second metrics. */
    void PrintPerSecondMetrics();

    /**
     * Report agents that never reached scheduling.
     *
     * @param localAddress Local AP address shown in the diagnostic.
     */
    void ReportUnscheduledAgents(Ipv4Address localAddress) const;

    std::map<std::string, Address> m_stationAddressByAgent;  ///< Destination station by agent.
    LegacyAgentOperations m_operationsByAgent;               ///< Legacy operation input by agent.
    DownlinkSchedulesByStation m_downlinkSchedulesByStation; ///< Ordered payloads by station.

    std::map<Address, Ptr<Socket>> m_socketByStation; ///< TCP socket by destination station.
    std::map<Address, bool> m_isConnectedByStation;   ///< Connection state by station.
    std::map<const void*, Address> m_socketToStation; ///< Station address by socket identity.
    std::map<Address, EventId> m_sendEventByStation;  ///< Pending send event by station.

    /** Per-station transport metrics. */
    struct StationMetrics
    {
        double currentCwnd{0}; ///< Most recently observed congestion window.
    };

    std::map<Address, StationMetrics> m_stationMetrics; ///< Transport metrics by station.

    uint64_t m_experimentStartMs{0}; ///< Simulation time corresponding to trace time zero.
    bool m_readyReported{false};     ///< Whether the readiness callback fired.
    bool m_trafficStarted{false};    ///< Whether payload scheduling started.
    Callback<void> m_readyCallback;  ///< Common-barrier readiness callback.
    std::vector<std::string> m_unscheduledAgentKeys; ///< Agent keys not yet scheduled.

    /** Metrics for one absolute simulation-second bucket. */
    struct PerSecondStats
    {
        uint64_t totalBytes{0};                     ///< Total accepted payload bytes.
        std::map<std::string, uint64_t> agentBytes; ///< Accepted bytes by agent.
        std::map<Address, uint64_t> stationBytes;   ///< Accepted bytes by station.
        double lastCwnd{0.0};                       ///< Last congestion window in bytes.
    };

    std::map<uint32_t, PerSecondStats> m_metricsByAbsoluteSecond; ///< Metrics by absolute second.

    TracedCallback<Address, std::string, uint32_t, Time> m_txTrace;        ///< Accepted sends.
    TracedCallback<Address, std::string, uint32_t, Time> m_appTxDropTrace; ///< Rejected bytes.

    /** Complete application-send trace callback signature. */
    using AgentSendCallback = void (*)(Address station,
                                       std::string agentKey,
                                       uint32_t bytes,
                                       Time startTime,
                                       Time endTime);
    TracedCallback<Address, std::string, uint32_t, Time, Time> m_agentSendTrace; ///< Sends.
};

} // namespace ns3

#endif // AP_GENERATOR_H
