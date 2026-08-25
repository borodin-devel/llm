#ifndef EXPERIMENT_OUTPUT_INTERNAL_H
#define EXPERIMENT_OUTPUT_INTERNAL_H

#include "experiment-window-output.h"

#include "ns3/json.hpp"

#include <iosfwd>
#include <optional>

namespace ns3
{

struct ScenarioConfig;

/**
 * Write one JSON scalar to a stream.
 *
 * @tparam T Scalar value type.
 * @param output Destination stream.
 * @param value Scalar value to encode.
 */
template <typename T>
void
WriteJsonScalar(std::ostream& output, const T& value)
{
    output << nlohmann::json(value).dump();
}

/**
 * Write one optional JSON scalar as its value or null.
 *
 * @tparam T Scalar value type.
 * @param output Destination stream.
 * @param value Optional scalar to encode.
 */
template <typename T>
void
WriteJsonScalar(std::ostream& output, const std::optional<T>& value)
{
    if (value)
    {
        WriteJsonScalar(output, *value);
    }
    else
    {
        output << "null";
    }
}

/** @param output Destination stream. @param distribution Distribution to serialize. */
void WriteSampleDistributionJson(std::ostream& output,
                                 const SampleDistributionOutput& distribution);

/** @param output Destination stream. @param direction General direction to serialize. */
void WriteGeneralDirectionJson(std::ostream& output, const GeneralDirectionOutput& direction);

/** @param output Destination stream. @param direction Application direction to serialize. */
void WriteAppDirectionJson(std::ostream& output, const AppDirectionOutput& direction);

/** @param output Destination stream. @param direction TCP direction to serialize. */
void WriteTcpDirectionJson(std::ostream& output, const TcpDirectionOutput& direction);

/** @param output Destination stream. @param direction MAC direction to serialize. */
void WriteMacDirectionJson(std::ostream& output, const MacDirectionOutput& direction);

/** @param output Destination stream. @param category PHY category to serialize. */
void WritePhyCategoryJson(std::ostream& output, const PhyCategoryOutput& category);

/** @param output Destination stream. @param statistics Entity statistics to serialize. */
void WriteEntityStatisticsJson(std::ostream& output, const EntityStatisticsOutput& statistics);

/** @param output Destination stream. @param entities AP records to serialize. */
void WriteAccessPointStatisticsArrayJson(std::ostream& output,
                                         const std::vector<AccessPointStatisticsOutput>& entities);

/** @param output Destination stream. @param entities station records to serialize. */
void WriteStationStatisticsArrayJson(std::ostream& output,
                                     const std::vector<StationStatisticsOutput>& entities);

/** @param output Destination stream. @param windows sparse windows to serialize. */
void WriteExperimentWindowsJson(std::ostream& output,
                                const std::vector<ExperimentWindowOutput>& windows);

/** @param output Destination stream. @param overall dense overall values to serialize. */
void WriteExperimentOverallJson(std::ostream& output, const ExperimentOverallOutput& overall);

/** @param output Destination stream. @param validation validation flags to serialize. */
void WriteExperimentValidationJson(std::ostream& output,
                                   const ExperimentValidationOutput& validation);

/**
 * Stream the complete schema-version-1 hierarchy without constructing a root JSON DOM.
 *
 * @param output Destination stream.
 * @param summary Finalized typed experiment summary.
 * @param configuration Effective scenario configuration.
 */
void WriteExperimentHierarchyJson(std::ostream& output,
                                  const UnifiedExperimentSummary& summary,
                                  const ScenarioConfig& configuration);

} // namespace ns3

#endif // EXPERIMENT_OUTPUT_INTERNAL_H
