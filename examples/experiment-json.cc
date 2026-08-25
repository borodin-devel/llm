#include "experiment-output-internal.h"
#include "scenario-config-internal.h"
#include "scenario-log.h"
#include "wifi-statistics-internal.h"
#include "wifi-statistics.h"

#include <fstream>
#include <stdexcept>
#include <string>

namespace ns3
{

static LogComponent& g_log = llm_example::GetScenarioLog();

void
WifiStatistics::WriteExperimentJson(const std::string& outputPath,
                                    const TransmissionSummary& transmissionSummary,
                                    const CrossLayerSummary& crossLayerSummary,
                                    const ScenarioConfig& configuration) const
{
    std::ofstream output(outputPath, std::ios::out | std::ios::noreplace);
    if (!output.is_open())
    {
        throw std::runtime_error("cannot exclusively create experiment output: '" + outputPath +
                                 "'");
    }

    output << "{\n  \"schema_version\": 1,\n"
           << "  \"measurement_semantics\": {\n"
           << "    \"mac_payload_source\": ";
    WriteJsonScalar(output, "PhyTxBegin+PhyTxPsduBegin/AppTxTag");
    output << ",\n    \"mac_payload_byte_semantics\": ";
    WriteJsonScalar(output, "tagged application payload observed at PHY; retransmissions included");
    output << ",\n    \"phy_data_rate_semantics\": ";
    WriteJsonScalar(output,
                    "airtime-weighted nominal WifiTxVector data rate of actual tagged PPDU "
                    "attempts; retransmissions included; PPDU airtime allocated by tagged "
                    "payload bytes");
    output << "\n  },\n";

    const WifiJsonValidation validation = WriteWifiStatisticsJsonMembers(output, *m_state);
    output << ",\n  \"transmission_summary\": ";
    WriteTransmissionSummaryJson(output, transmissionSummary);
    output << ",\n  \"cross_layer_summary\": ";
    WriteCrossLayerSummaryJson(output, crossLayerSummary);
    output << ",\n  \"validation\": {\n"
           << "    \"window_payload_totals_consistent\": ";
    WriteJsonScalar(output, validation.windowPayloadTotalsConsistent);
    output << ",\n    \"summary_payload_totals_consistent\": ";
    WriteJsonScalar(output, validation.summaryPayloadTotalsConsistent);
    output << "\n  },\n  \"experiment_metadata\": {\n"
           << "    \"configuration\": ";
    WriteEffectiveConfigurationJson(output, configuration);
    output << "\n  }\n}\n";

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
