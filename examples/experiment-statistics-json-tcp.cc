#include "experiment-output-internal.h"

#include <ostream>

namespace ns3
{

void
WriteTcpDirectionJson(std::ostream& output, const TcpDirectionOutput& direction)
{
    output << "{\"connections\":[";
    bool first = true;
    for (const auto& connection : direction.connections)
    {
        output << (first ? "" : ",") << "{\"peer_node_id\":";
        WriteJsonScalar(output, connection.peerNodeId);
        output << ",\"peer_ipv4\":";
        WriteJsonScalar(output, connection.peerIpv4);
        output << ",\"congestion_window_observation_duration_us\":";
        WriteJsonScalar(output, connection.congestionWindowObservationDurationUs);
        output << ",\"average_congestion_window_bytes\":";
        WriteJsonScalar(output, connection.averageCongestionWindowBytes);
        output << ",\"last_congestion_window_bytes\":";
        WriteJsonScalar(output, connection.lastCongestionWindowBytes);
        output << ",\"round_trip_time\":";
        WriteSampleDistributionJson(output, connection.roundTripTime);
        output << '}';
        first = false;
    }
    output << "]}";
}

} // namespace ns3
