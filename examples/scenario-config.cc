#include "scenario-config.h"

#include <cmath>
#include <exception>
#include <ostream>
#include <stdexcept>

namespace ns3
{

namespace
{

bool
IsSupportedBandwidth(int bandwidthMhz)
{
    return bandwidthMhz == 20 || bandwidthMhz == 40 || bandwidthMhz == 80 || bandwidthMhz == 160;
}

std::string
InvalidDurationMessage(const std::string& value)
{
    return "Invalid experiment_time: " + value +
           ". Expected 'auto' or a positive number of seconds.";
}

} // namespace

ScenarioArgumentResult
ParseScenarioArguments(const std::vector<std::string>& arguments)
{
    ScenarioArgumentResult result;
    if (arguments.empty())
    {
        result.printUsage = true;
        return result;
    }

    result.config.tracePath = arguments[0];
    if (arguments.size() >= 2)
    {
        // Preserve the sample's existing std::stoi behavior for malformed input.
        result.config.bandwidthMhz = std::stoi(arguments[1]);
    }
    if (arguments.size() >= 3)
    {
        result.config.statisticsOutputPath = arguments[2];
    }
    if (arguments.size() >= 4 && arguments[3] != "auto")
    {
        try
        {
            std::size_t parsedCharacters = 0;
            const double fixedDurationSeconds = std::stod(arguments[3], &parsedCharacters);
            if (parsedCharacters != arguments[3].size() || !std::isfinite(fixedDurationSeconds) ||
                fixedDurationSeconds <= 0.0)
            {
                throw std::invalid_argument("invalid experiment time");
            }

            result.config.automaticDuration = false;
            result.config.fixedDurationMs = fixedDurationSeconds * 1000.0;
        }
        catch (const std::exception&)
        {
            result.error = InvalidDurationMessage(arguments[3]);
            return result;
        }
    }
    if (arguments.size() > 4)
    {
        result.error = "Too many command-line arguments.";
        return result;
    }
    if (!IsSupportedBandwidth(result.config.bandwidthMhz))
    {
        result.error = "Unsupported bandwidth: " + std::to_string(result.config.bandwidthMhz) +
                       " MHz. Expected 20, 40, 80 or 160.";
        return result;
    }

    result.valid = true;
    return result;
}

void
PrintScenarioUsage(std::ostream& output, const std::string& programName)
{
    output << "Usage: " << programName
           << " <traces.json> [bandwidth_mhz] [stats_output.json] [experiment_time]"
           << "\n  bandwidth_mhz: 20, 40, 80 or 160 (default: 20)"
           << "\n  stats_output.json: default mac-node-stats.json"
           << "\n  experiment_time: auto (JSON duration + 2s, default) or fixed seconds > 0"
           << std::endl;
}

} // namespace ns3
