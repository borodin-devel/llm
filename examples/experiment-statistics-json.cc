#include "experiment-output-internal.h"

#include <ostream>

namespace ns3
{

void
WriteExperimentWindowsJson(std::ostream& output, const std::vector<ExperimentWindowOutput>& windows)
{
    output << '[';
    bool first = true;
    for (const auto& window : windows)
    {
        output << (first ? "" : ",") << "{\"window_index\":";
        WriteJsonScalar(output, window.windowIndex);
        output << ",\"window_start_ms\":";
        WriteJsonScalar(output, window.windowStartMs);
        output << ",\"window_duration_ms\":";
        WriteJsonScalar(output, window.windowDurationMs);
        output << ",\"access_points\":";
        WriteAccessPointStatisticsArrayJson(output, window.accessPoints);
        output << ",\"stations\":";
        WriteStationStatisticsArrayJson(output, window.stations);
        output << '}';
        first = false;
    }
    output << ']';
}

void
WriteExperimentOverallJson(std::ostream& output, const ExperimentOverallOutput& overall)
{
    output << "{\"access_points\":";
    WriteAccessPointStatisticsArrayJson(output, overall.accessPoints);
    output << ",\"stations\":";
    WriteStationStatisticsArrayJson(output, overall.stations);
    output << '}';
}

void
WriteExperimentValidationJson(std::ostream& output, const ExperimentValidationOutput& validation)
{
    output << "{\"entity_inventory_references_valid\":";
    WriteJsonScalar(output, validation.entityInventoryReferencesValid);
    output << ",\"app_agent_totals_consistent\":";
    WriteJsonScalar(output, validation.appAgentTotalsConsistent);
    output << ",\"app_peer_totals_consistent\":";
    WriteJsonScalar(output, validation.appPeerTotalsConsistent);
    output << ",\"mac_peer_totals_consistent\":";
    WriteJsonScalar(output, validation.macPeerTotalsConsistent);
    output << ",\"phy_peer_totals_consistent\":";
    WriteJsonScalar(output, validation.phyPeerTotalsConsistent);
    output << ",\"ap_station_sender_totals_consistent\":";
    WriteJsonScalar(output, validation.apStationSenderTotalsConsistent);
    output << ",\"overall_matches_windows\":";
    WriteJsonScalar(output, validation.overallMatchesWindows);
    output << ",\"unique_phy_payload_within_tagged_payload\":";
    WriteJsonScalar(output, validation.uniquePhyPayloadWithinTaggedPayload);
    output << '}';
}

} // namespace ns3
