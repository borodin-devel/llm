#include "statistics/json/writer.h"

namespace ns3
{

void
WriteEntityStatisticsJson(JsonWriter& writer, const EntityStatisticsOutput& statistics)
{
    writer.Key("general_stats");
    writer.BeginObject();
    writer.Key("uplink");
    WriteGeneralDirectionJson(writer, statistics.generalStats.uplink);
    writer.Key("downlink");
    WriteGeneralDirectionJson(writer, statistics.generalStats.downlink);
    writer.EndObject();
    writer.Key("app_stats");
    writer.BeginObject();
    writer.Key("uplink");
    WriteAppDirectionJson(writer, statistics.appStats.uplink);
    writer.Key("downlink");
    WriteAppDirectionJson(writer, statistics.appStats.downlink);
    writer.EndObject();
    writer.Key("tcp_stats");
    writer.BeginObject();
    writer.Key("uplink");
    WriteTcpDirectionJson(writer, statistics.tcpStats.uplink);
    writer.Key("downlink");
    WriteTcpDirectionJson(writer, statistics.tcpStats.downlink);
    writer.EndObject();
    writer.Key("mac_stats");
    writer.BeginObject();
    writer.Key("uplink");
    WriteMacDirectionJson(writer, statistics.macStats.uplink);
    writer.Key("downlink");
    WriteMacDirectionJson(writer, statistics.macStats.downlink);
    writer.EndObject();
    writer.Key("phy_stats");
    WritePhyCategoryJson(writer, statistics.phyStats);
}

void
WriteAccessPointStatisticsArrayJson(JsonWriter& writer,
                                    const std::vector<AccessPointStatisticsOutput>& entities)
{
    writer.BeginArray();
    for (const auto& entity : entities)
    {
        writer.BeginObject();
        writer.Key("access_point_id");
        writer.Value(entity.accessPointId);
        writer.Key("node_id");
        writer.Value(entity.nodeId);
        writer.Key("node_label");
        writer.Value(entity.nodeLabel);
        writer.Key("ipv4");
        writer.Value(entity.ipv4);
        WriteEntityStatisticsJson(writer, entity.statistics);
        writer.EndObject();
    }
    writer.EndArray();
}

void
WriteStationStatisticsArrayJson(JsonWriter& writer,
                                const std::vector<StationStatisticsOutput>& entities)
{
    writer.BeginArray();
    for (const auto& entity : entities)
    {
        writer.BeginObject();
        writer.Key("access_point_id");
        writer.Value(entity.accessPointId);
        writer.Key("station_index");
        writer.Value(entity.stationIndex);
        writer.Key("node_id");
        writer.Value(entity.nodeId);
        writer.Key("node_label");
        writer.Value(entity.nodeLabel);
        writer.Key("ipv4");
        writer.Value(entity.ipv4);
        WriteEntityStatisticsJson(writer, entity.statistics);
        writer.EndObject();
    }
    writer.EndArray();
}

} // namespace ns3
