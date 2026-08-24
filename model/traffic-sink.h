// model/traffic-sink.h
//
// Traffic Sink Application - TCP receiver for station
// Based on ns3::PacketSink pattern (inherits SinkApplication)
//

#ifndef TRAFFIC_SINK_H
#define TRAFFIC_SINK_H

#include "ns3/sink-application.h"
#include "ns3/event-id.h"
#include "ns3/traced-callback.h"
#include "ns3/traced-value.h"

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
 * @brief TCP traffic sink that receives and logs agent payloads.
 *
 * Receives TCP connections from TrafficGenerator applications and logs
 * received bytes per agent. Tracks per-second throughput and channel
 * utilization metrics.
 *
 * Metrics tracked:
 *   - total bytes received
 *   - per-agent bytes received
 *   - per-second throughput (bps)
 */
class TrafficSink : public SinkApplication
{
  public:
    /**
     * @brief Get the type ID.
     * @return the object TypeId
     */
    static TypeId GetTypeId();

    TrafficSink();
    ~TrafficSink() override;

  private:
    void DoDispose() override;
    void DoStartApplication() override;
    void DoStopApplication() override;

    void HandleRead(Ptr<Socket> socket);
    void HandleAccept(Ptr<Socket> socket, const Address &from);
    void HandlePeerClose(Ptr<Socket> socket);
    void HandleError(Ptr<Socket> socket);
    void LogPerSecondMetrics();
    void RecordInterArrivalTime(Time now);

    // Accepted sockets (one per TCP connection)
    std::vector<Ptr<Socket>> m_acceptedSockets;

    // Metrics: total bytes received
    uint64_t m_totalReceived;

    // Metrics: per-agent bytes received
    std::map<int64_t, uint64_t> m_agentBytesReceived;

    // Metrics: per-second tracking
    std::map<int64_t, uint64_t> m_agentBytesThisSecond;
    Time m_lastMetricCheckTime;

    // Metrics: inter-arrival time samples (us)
    std::vector<double> m_iatSamplesUs;
    Time m_lastPacketTime;

    // Traced callbacks
    TracedCallback<uint64_t, Address> m_rxTraceCustom;
};

} // namespace ns3

#endif /* TRAFFIC_SINK_H */
