#include "scenario-config-internal.h"

#include <ostream>

namespace ns3
{

void
WriteEffectiveConfigurationJson(std::ostream& output, const ScenarioConfig& configuration)
{
    output << '{';
    std::string_view activeSection;
    bool firstSection = true;
    bool firstField = true;
    for (const auto& option : GetScenarioConfigOptions())
    {
        const auto separator = option.tomlPath.find('.');
        if (separator == std::string::npos || separator == 0 ||
            separator == option.tomlPath.size() - 1)
        {
            throw ScenarioConfigError("invalid configuration path: " + option.tomlPath);
        }

        const auto section = std::string_view(option.tomlPath).substr(0, separator);
        const auto field = std::string_view(option.tomlPath).substr(separator + 1);
        if (section != activeSection)
        {
            if (!firstSection)
            {
                output << '}';
            }
            output << (firstSection ? "" : ",") << nlohmann::json(section).dump() << ":{";
            activeSection = section;
            firstSection = false;
            firstField = true;
        }
        if (!firstField)
        {
            output << ',';
        }
        output << nlohmann::json(field).dump() << ':' << option.readJson(configuration).dump();
        firstField = false;
    }
    output << (firstSection ? "" : "}") << '}';
}

} // namespace ns3
