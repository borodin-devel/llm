#include "statistics/json/writer.h"

#include <ostream>

namespace ns3
{

void
WriteEntityStatisticsJson(std::ostream& output, const EntityStatisticsOutput& statistics)
{
    output << "\"general_stats\":{\"uplink\":";
    WriteGeneralDirectionJson(output, statistics.generalStats.uplink);
    output << ",\"downlink\":";
    WriteGeneralDirectionJson(output, statistics.generalStats.downlink);
    output << "},\"app_stats\":{\"uplink\":";
    WriteAppDirectionJson(output, statistics.appStats.uplink);
    output << ",\"downlink\":";
    WriteAppDirectionJson(output, statistics.appStats.downlink);
    output << "},\"tcp_stats\":{\"uplink\":";
    WriteTcpDirectionJson(output, statistics.tcpStats.uplink);
    output << ",\"downlink\":";
    WriteTcpDirectionJson(output, statistics.tcpStats.downlink);
    output << "},\"mac_stats\":{\"uplink\":";
    WriteMacDirectionJson(output, statistics.macStats.uplink);
    output << ",\"downlink\":";
    WriteMacDirectionJson(output, statistics.macStats.downlink);
    output << "},\"phy_stats\":";
    WritePhyCategoryJson(output, statistics.phyStats);
}

void
WriteAccessPointStatisticsArrayJson(std::ostream& output,
                                    const std::vector<AccessPointStatisticsOutput>& entities)
{
    output << '[';
    bool first = true;
    for (const auto& entity : entities)
    {
        output << (first ? "" : ",") << "{\"access_point_id\":";
        WriteJsonScalar(output, entity.accessPointId);
        output << ",\"node_id\":";
        WriteJsonScalar(output, entity.nodeId);
        output << ",\"node_label\":";
        WriteJsonScalar(output, entity.nodeLabel);
        output << ",\"ipv4\":";
        WriteJsonScalar(output, entity.ipv4);
        output << ',';
        WriteEntityStatisticsJson(output, entity.statistics);
        output << '}';
        first = false;
    }
    output << ']';
}

void
WriteStationStatisticsArrayJson(std::ostream& output,
                                const std::vector<StationStatisticsOutput>& entities)
{
    output << '[';
    bool first = true;
    for (const auto& entity : entities)
    {
        output << (first ? "" : ",") << "{\"access_point_id\":";
        WriteJsonScalar(output, entity.accessPointId);
        output << ",\"station_index\":";
        WriteJsonScalar(output, entity.stationIndex);
        output << ",\"node_id\":";
        WriteJsonScalar(output, entity.nodeId);
        output << ",\"node_label\":";
        WriteJsonScalar(output, entity.nodeLabel);
        output << ",\"ipv4\":";
        WriteJsonScalar(output, entity.ipv4);
        output << ',';
        WriteEntityStatisticsJson(output, entity.statistics);
        output << '}';
        first = false;
    }
    output << ']';
}

} // namespace ns3
