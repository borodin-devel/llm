#include "../statistics/json/writer.h"
#include "config-internal.h"

namespace ns3
{

void
WriteEffectiveSaturatedTcpConfigurationJson(JsonWriter& writer, const SaturatedTcpConfig& config)
{
    writer.BeginObject();
    std::string_view activeSection;
    for (const auto& option : GetSaturatedTcpConfigOptions())
    {
        const auto [section, field] = SplitSaturatedTcpConfigPath(option.tomlPath);
        if (section != activeSection)
        {
            if (!activeSection.empty())
            {
                writer.EndObject();
            }
            writer.Key(section);
            writer.BeginObject();
            activeSection = section;
        }
        writer.Key(field);
        writer.Value(option.readJson(config));
    }
    if (!activeSection.empty())
    {
        writer.EndObject();
    }
    writer.EndObject();
}

} // namespace ns3
