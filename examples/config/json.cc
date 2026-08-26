#include "internal.h"

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

} // namespace ns3
