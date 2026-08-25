#ifndef TRAFFIC_FLOW_MONITOR_H
#define TRAFFIC_FLOW_MONITOR_H

#include "experiment-output.h"

#include "ns3/ptr.h"

#include <memory>
#include <string>

namespace ns3
{

class Packet;
class TrafficCoordinator;
class WifiStatistics;
struct TrafficFlowMonitorState;

/** Track device-level TCP payload transmission and reception times. */
class TrafficFlowMonitor
{
  public:
    /**
     * Construct a monitor for one scenario.
     *
     * @param coordinator Traffic epoch owner.
     * @param wifiStatistics Wi-Fi statistics receiving MAC payload samples.
     */
    TrafficFlowMonitor(const TrafficCoordinator& coordinator, WifiStatistics& wifiStatistics);
    ~TrafficFlowMonitor();

    /** Connect to the transmit and receive traces of all Wi-Fi devices. */
    void ConnectDeviceTraces();

    /**
     * Record one device transmit trace event.
     *
     * @param context ns-3 trace context.
     * @param packet Transmitted packet.
     */
    void RecordDeviceTx(std::string context, Ptr<const Packet> packet);

    /**
     * Record one device receive trace event.
     *
     * @param context ns-3 trace context.
     * @param packet Received packet.
     */
    void RecordDeviceRx(std::string context, Ptr<const Packet> packet);

    /**
     * Build aggregate transmission measurements for every observed sender.
     *
     * @return Typed per-sender transmission measurements.
     */
    TransmissionSummary BuildTransmissionSummary() const;

    /** Print aggregate transmission time and payload per sender. */
    void PrintTransmissionTimePerSender() const;

  private:
    const TrafficCoordinator& m_coordinator;          ///< Traffic epoch owner.
    WifiStatistics& m_wifiStatistics;                 ///< Cross-layer statistics receiver.
    std::unique_ptr<TrafficFlowMonitorState> m_state; ///< Per-flow timestamps and byte counts.
};

} // namespace ns3

#endif // TRAFFIC_FLOW_MONITOR_H
