#ifndef TRAFFIC_FLOW_MONITOR_INTERNAL_H
#define TRAFFIC_FLOW_MONITOR_INTERNAL_H

#include "experiment-output.h"

#include <cstdint>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace ns3
{

/** Key that identifies a TCP payload flow observed at a Wi-Fi device. */
struct TrafficFlowKey
{
    std::string sourceIp;        ///< Source IPv4 address.
    uint16_t sourcePort;         ///< Source TCP port.
    std::string destinationIp;   ///< Destination IPv4 address.
    uint16_t destinationPort;    ///< Destination TCP port.
    uint32_t payloadBytes;       ///< TCP payload size in bytes.

    /**
     * Order keys lexicographically by all flow fields.
     *
     * @param other Key to compare.
     * @return `true` when this key precedes @p other.
     */
    bool operator<(const TrafficFlowKey& other) const
    {
        return std::tie(sourceIp, sourcePort, destinationIp, destinationPort, payloadBytes) <
               std::tie(other.sourceIp,
                        other.sourcePort,
                        other.destinationIp,
                        other.destinationPort,
                        other.payloadBytes);
    }
};

/** Trace data used to construct a transmission summary. */
struct TrafficFlowMonitorState
{
    std::map<std::string, std::vector<uint64_t>> transmittedBytesBySource; ///< Payload samples by sender.
    std::map<TrafficFlowKey, std::vector<uint64_t>> transmitTimestampsByFlow; ///< TX timestamps in us.
    std::map<TrafficFlowKey, std::vector<uint64_t>> receiveTimestampsByFlow;  ///< RX timestamps in us.
};

/**
 * Build a typed transmission summary from collected trace data.
 *
 * @param state Per-flow timestamps and payload samples.
 * @return Per-sender transmission measurements.
 */
TransmissionSummary BuildTransmissionSummary(const TrafficFlowMonitorState& state);

} // namespace ns3

#endif // TRAFFIC_FLOW_MONITOR_INTERNAL_H
