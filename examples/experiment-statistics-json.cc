#include "statistics/json/writer.h"

namespace ns3
{

void
WriteExperimentWindowsJson(JsonWriter& writer, const std::vector<ExperimentWindowOutput>& windows)
{
    writer.BeginArray();
    for (const auto& window : windows)
    {
        writer.BeginObject();
        writer.Key("window_index");
        writer.Value(window.windowIndex);
        writer.Key("window_start_ms");
        writer.Value(window.windowStartMs);
        writer.Key("window_duration_ms");
        writer.Value(window.windowDurationMs);
        writer.Key("access_points");
        WriteAccessPointStatisticsArrayJson(writer, window.accessPoints);
        writer.Key("stations");
        WriteStationStatisticsArrayJson(writer, window.stations);
        writer.EndObject();
    }
    writer.EndArray();
}

void
WriteExperimentOverallJson(JsonWriter& writer, const ExperimentOverallOutput& overall)
{
    writer.BeginObject();
    writer.Key("access_points");
    WriteAccessPointStatisticsArrayJson(writer, overall.accessPoints);
    writer.Key("stations");
    WriteStationStatisticsArrayJson(writer, overall.stations);
    writer.EndObject();
}

void
WriteExperimentValidationJson(JsonWriter& writer, const ExperimentValidationOutput& validation)
{
    writer.BeginObject();
    writer.Key("entity_inventory_references_valid");
    writer.Value(validation.entityInventoryReferencesValid);
    writer.Key("app_agent_totals_consistent");
    writer.Value(validation.appAgentTotalsConsistent);
    writer.Key("app_peer_totals_consistent");
    writer.Value(validation.appPeerTotalsConsistent);
    writer.Key("mac_peer_totals_consistent");
    writer.Value(validation.macPeerTotalsConsistent);
    writer.Key("phy_peer_totals_consistent");
    writer.Value(validation.phyPeerTotalsConsistent);
    writer.Key("ap_station_sender_totals_consistent");
    writer.Value(validation.apStationSenderTotalsConsistent);
    writer.Key("overall_matches_windows");
    writer.Value(validation.overallMatchesWindows);
    writer.Key("unique_phy_payload_within_tagged_payload");
    writer.Value(validation.uniquePhyPayloadWithinTaggedPayload);
    writer.EndObject();
}

} // namespace ns3
