#include "statistics/json/writer.h"

namespace ns3
{

void
WriteTcpDirectionJson(JsonWriter& writer, const TcpDirectionOutput& direction)
{
    writer.BeginObject();
    writer.Key("connections");
    writer.BeginArray();
    for (const auto& connection : direction.connections)
    {
        writer.BeginObject();
        writer.Key("peer_node_id");
        writer.Value(connection.peerNodeId);
        writer.Key("peer_ipv4");
        writer.Value(connection.peerIpv4);
        writer.Key("congestion_window_observation_duration_us");
        writer.Value(connection.congestionWindowObservationDurationUs);
        writer.Key("average_congestion_window_bytes");
        if (connection.averageCongestionWindowBytes)
        {
            writer.Value(*connection.averageCongestionWindowBytes);
        }
        else
        {
            writer.Null();
        }
        writer.Key("last_congestion_window_bytes");
        if (connection.lastCongestionWindowBytes)
        {
            writer.Value(*connection.lastCongestionWindowBytes);
        }
        else
        {
            writer.Null();
        }
        writer.Key("round_trip_time");
        WriteSampleDistributionJson(writer, connection.roundTripTime);
        writer.EndObject();
    }
    writer.EndArray();
    writer.EndObject();
}

} // namespace ns3
