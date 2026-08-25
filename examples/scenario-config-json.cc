#include "scenario-config-internal.h"

#include <ostream>

namespace ns3
{

std::pair<std::string_view, std::string_view>
SplitScenarioConfigPath(std::string_view path)
{
    const auto separator = path.find('.');
    if (separator == std::string_view::npos || separator == 0 || separator == path.size() - 1 ||
        path.find('.', separator + 1) != std::string_view::npos)
    {
        throw ScenarioConfigError("invalid configuration path: " + std::string(path));
    }
    return {path.substr(0, separator), path.substr(separator + 1)};
}

void
WriteEffectiveConfigurationJson(std::ostream& output, const ScenarioConfig& configuration)
{
    output << '{';
    std::string_view activeSection;
    bool firstSection = true;
    bool firstField = true;
    for (const auto& option : GetScenarioConfigOptions())
    {
        const auto [section, field] = SplitScenarioConfigPath(option.tomlPath);
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
