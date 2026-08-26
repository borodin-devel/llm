#include "writer.h"

#include "../../config/internal.h"
#include "../../runtime/log.h"
#include "../experiment-statistics.h"

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
WriteMeasurementSemanticsJson(JsonWriter& writer)
{
    writer.BeginObject();
    writer.Key("access_point_role");
    writer.Value("BSS parent aggregate");
    writer.Key("station_role");
    writer.Value("per-station child detail");
    writer.Key("parent_child_duplication");
    writer.Value("intentional");
    writer.Key("mac_tcp_payload_bytes");
    writer.Value("header-based estimates");
    writer.Key("phy_tagged_payload_bytes");
    writer.Value("attempts and retransmissions included");
    writer.Key("phy_unique_tagged_payload_bytes");
    writer.Value("first tagged MPDU transmissions only");
    writer.Key("phy_average_data_rate");
    writer.Value("airtime-weighted");
    writer.Key("congestion_window");
    writer.Value("time-weighted per connection");
    writer.Key("sample_distributions");
    writer.Value("sample-weighted");
    writer.Key("sparse_window_absence");
    writer.Value("zero activity");
    writer.Key("undefined_derived_values");
    writer.Null();
    writer.EndObject();
}

} // namespace

void
WriteExperimentHierarchyJson(std::ostream& output,
                             const UnifiedExperimentSummary& summary,
                             const ScenarioConfig& configuration)
{
    const ExperimentJsonSections sections{
        WriteMeasurementSemanticsJson,
        [&configuration](JsonWriter& writer) {
            WriteEffectiveConfigurationJson(writer, configuration);
        },
    };
    WriteExperimentHierarchyJson(output, summary, sections);
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
