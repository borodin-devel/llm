#include "statistics/json/writer.h"
#include "experiment-statistics.h"
#include "scenario-config-internal.h"
#include "scenario-log.h"

#include <fstream>
#include <ostream>
#include <stdexcept>
#include <string>

namespace ns3
{

namespace
{

LogComponent& g_log = llm_example::GetScenarioLog();

void
WriteMeasurementSemanticsJson(std::ostream& output)
{
    output << "{\"access_point_role\":";
    WriteJsonScalar(output, "BSS parent aggregate");
    output << ",\"station_role\":";
    WriteJsonScalar(output, "per-station child detail");
    output << ",\"parent_child_duplication\":";
    WriteJsonScalar(output, "intentional");
    output << ",\"mac_tcp_payload_bytes\":";
    WriteJsonScalar(output, "header-based estimates");
    output << ",\"phy_tagged_payload_bytes\":";
    WriteJsonScalar(output, "attempts and retransmissions included");
    output << ",\"phy_unique_tagged_payload_bytes\":";
    WriteJsonScalar(output, "first tagged MPDU transmissions only");
    output << ",\"phy_average_data_rate\":";
    WriteJsonScalar(output, "airtime-weighted");
    output << ",\"congestion_window\":";
    WriteJsonScalar(output, "time-weighted per connection");
    output << ",\"sample_distributions\":";
    WriteJsonScalar(output, "sample-weighted");
    output << ",\"sparse_window_absence\":";
    WriteJsonScalar(output, "zero activity");
    output << ",\"undefined_derived_values\":";
    WriteJsonScalar(output, nullptr);
    output << '}';
}

void
WriteIdentityJson(std::ostream& output, const ExperimentEntityIdentity& identity)
{
    output << "{\"access_point_id\":";
    WriteJsonScalar(output, identity.accessPointId);
    if (identity.kind == ExperimentEntityKind::STATION)
    {
        output << ",\"station_index\":";
        WriteJsonScalar(output, identity.stationIndex);
    }
    output << ",\"node_id\":";
    WriteJsonScalar(output, identity.nodeId);
    output << ",\"node_label\":";
    WriteJsonScalar(output, identity.nodeLabel);
    output << ",\"ipv4\":";
    WriteJsonScalar(output, identity.ipv4);
    output << '}';
}

void
WriteInventoryArrayJson(std::ostream& output,
                        const std::vector<ExperimentEntityIdentity>& inventory)
{
    output << '[';
    bool first = true;
    for (const auto& identity : inventory)
    {
        output << (first ? "" : ",");
        WriteIdentityJson(output, identity);
        first = false;
    }
    output << ']';
}

} // namespace

void
WriteExperimentHierarchyJson(std::ostream& output,
                             const UnifiedExperimentSummary& summary,
                             const ScenarioConfig& configuration)
{
    output << "{\"schema_version\":1,\"measurement_semantics\":";
    WriteMeasurementSemanticsJson(output);
    output << ",\"statistics_window_ms\":";
    WriteJsonScalar(output, summary.statisticsWindowMs);
    output << ",\"windows\":";
    WriteExperimentWindowsJson(output, summary.windows);
    output << ",\"overall\":";
    WriteExperimentOverallJson(output, summary.overall);
    output << ",\"validation\":";
    WriteExperimentValidationJson(output, summary.validation);
    output << ",\"experiment_metadata\":{\"configuration\":";
    WriteEffectiveConfigurationJson(output, configuration);
    output << ",\"entity_inventory\":{\"access_points\":";
    WriteInventoryArrayJson(output, summary.accessPointInventory);
    output << ",\"stations\":";
    WriteInventoryArrayJson(output, summary.stationInventory);
    output << "}}}";
}

void
ExperimentStatistics::WriteExperimentJson(const std::string& outputPath,
                                          const ScenarioConfig& configuration)
{
    Finalize();
    const UnifiedExperimentSummary summary = BuildUnifiedExperimentSummary();

    std::ofstream output(outputPath, std::ios::out | std::ios::noreplace);
    if (!output.is_open())
    {
        throw std::runtime_error("cannot exclusively create experiment output: '" + outputPath +
                                 "'");
    }

    WriteExperimentHierarchyJson(output, summary, configuration);
    output << '\n';
    if (!output)
    {
        throw std::runtime_error("failed to write experiment output: '" + outputPath + "'");
    }
    output.flush();
    if (!output)
    {
        throw std::runtime_error("failed to flush experiment output: '" + outputPath + "'");
    }
    output.close();
    if (output.fail())
    {
        throw std::runtime_error("failed to close experiment output: '" + outputPath + "'");
    }

    NS_LOG_INFO("Experiment output written to " << outputPath);
}

} // namespace ns3
