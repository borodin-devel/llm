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

std::ostream&
operator<<(std::ostream& output, DurationMode durationMode)
{
    switch (durationMode)
    {
    case DurationMode::AUTO:
        return output << "AUTO";
    case DurationMode::FIXED:
        return output << "FIXED";
    }
    return output << "unknown DurationMode";
}

std::ostream&
operator<<(std::ostream& output, WifiBandConfig band)
{
    switch (band)
    {
    case WifiBandConfig::BAND_2_4_GHZ:
        return output << "BAND_2_4_GHZ";
    case WifiBandConfig::BAND_5_GHZ:
        return output << "BAND_5_GHZ";
    case WifiBandConfig::BAND_6_GHZ:
        return output << "BAND_6_GHZ";
    }
    return output << "unknown WifiBandConfig";
}

ScenarioArgumentResult
ParseScenarioArguments(const std::vector<std::string>& arguments)
{
    ScenarioArgumentResult result;
    if (arguments.empty())
    {
        result.printUsage = true;
        return result;
    }

    result.config.general.traceFile = arguments[0];
    if (arguments.size() >= 2)
    {
        // Preserve the sample's existing std::stoi behavior for malformed input.
        result.config.wifi.bandwidthMhz = std::stoi(arguments[1]);
    }
    if (arguments.size() >= 3)
    {
        result.config.general.outputName = arguments[2];
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

            result.config.simulation.durationMode = DurationMode::FIXED;
            result.config.simulation.fixedDurationSeconds = fixedDurationSeconds;
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
    if (!IsSupportedBandwidth(result.config.wifi.bandwidthMhz))
    {
        result.error = "Unsupported bandwidth: " + std::to_string(result.config.wifi.bandwidthMhz) +
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
