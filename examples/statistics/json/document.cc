#include "writer.h"

#include <stdexcept>
#include <vector>

namespace ns3
{

namespace
{

void
WriteIdentityJson(JsonWriter& writer, const ExperimentEntityIdentity& identity)
{
    writer.BeginObject();
    writer.Key("access_point_id");
    writer.Value(identity.accessPointId);
    if (identity.kind == ExperimentEntityKind::STATION)
    {
        writer.Key("station_index");
        if (identity.stationIndex)
        {
            writer.Value(*identity.stationIndex);
        }
        else
        {
            writer.Null();
        }
    }
    writer.Key("node_id");
    writer.Value(identity.nodeId);
    writer.Key("node_label");
    writer.Value(identity.nodeLabel);
    writer.Key("ipv4");
    writer.Value(identity.ipv4);
    writer.EndObject();
}

void
WriteInventoryArrayJson(JsonWriter& writer, const std::vector<ExperimentEntityIdentity>& inventory)
{
    writer.BeginArray();
    for (const auto& identity : inventory)
    {
        WriteIdentityJson(writer, identity);
    }
    writer.EndArray();
}

} // namespace

void
WriteExperimentHierarchyJson(std::ostream& output,
                             const UnifiedExperimentSummary& summary,
                             const ExperimentJsonSections& sections)
{
    if (!sections.writeMeasurementSemantics)
    {
        throw std::invalid_argument("measurement-semantics JSON callback must be set");
    }
    if (!sections.writeConfiguration)
    {
        throw std::invalid_argument("configuration JSON callback must be set");
    }

    JsonWriter writer(output);
    writer.BeginObject();
    writer.Key("schema_version");
    writer.Value(2);
    writer.Key("measurement_semantics");
    sections.writeMeasurementSemantics(writer);
    writer.Key("statistics_window_ms");
    writer.Value(summary.statisticsWindowMs);
    writer.Key("windows");
    WriteExperimentWindowsJson(writer, summary.windows);
    writer.Key("overall");
    WriteExperimentOverallJson(writer, summary.overall);
    writer.Key("validation");
    WriteExperimentValidationJson(writer, summary.validation);
    writer.Key("experiment_metadata");
    writer.BeginObject();
    writer.Key("configuration");
    sections.writeConfiguration(writer);
    writer.Key("entity_inventory");
    writer.BeginObject();
    writer.Key("access_points");
    WriteInventoryArrayJson(writer, summary.accessPointInventory);
    writer.Key("stations");
    WriteInventoryArrayJson(writer, summary.stationInventory);
    writer.EndObject();
    writer.EndObject();
    writer.EndObject();
    writer.Finish();
}

} // namespace ns3
