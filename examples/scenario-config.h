#ifndef SCENARIO_CONFIG_H
#define SCENARIO_CONFIG_H

#include <iosfwd>
#include <string>
#include <vector>

namespace ns3
{

/** User-configurable sample-scenario settings. */
struct ScenarioConfig
{
    std::string tracePath;                                   ///< Input JSON trace path.
    int bandwidthMhz{20};                                    ///< Wi-Fi channel width in MHz.
    std::string statisticsOutputPath{"mac-node-stats.json"}; ///< Statistics JSON path.
    bool automaticDuration{true}; ///< Whether duration follows the input trace.
    double fixedDurationMs{0.0};  ///< Fixed duration in milliseconds when selected.
    int bssCount{3};              ///< Number of isolated AP groups.
    int stationsPerBss{30};       ///< Number of stations in each AP group.
};

/** Result of parsing sample-scenario arguments. */
struct ScenarioArgumentResult
{
    bool valid{false};      ///< Whether config is ready for use.
    bool printUsage{false}; ///< Whether the caller should print usage.
    ScenarioConfig config;  ///< Parsed settings and defaults.
    std::string error;      ///< Validation error, or empty on success.
};

/**
 * Parse command-line arguments excluding the executable name.
 *
 * @param arguments Ordered command-line arguments.
 * @return Parsed settings or a validation error.
 */
ScenarioArgumentResult ParseScenarioArguments(const std::vector<std::string>& arguments);

/**
 * Print sample-scenario command-line usage.
 *
 * @param output Destination stream.
 * @param programName Executable name shown in the command synopsis.
 */
void PrintScenarioUsage(std::ostream& output, const std::string& programName);

} // namespace ns3

#endif // SCENARIO_CONFIG_H
