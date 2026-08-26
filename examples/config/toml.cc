#include "internal.h"
#include "scenario-config.h"

#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>

namespace ns3
{

namespace
{

using ScenarioSchema = std::map<std::string, std::set<std::string>>;

std::pair<std::string_view, std::string_view>
SplitTomlPath(std::string_view path)
{
    const auto separator = path.find('.');
    return {path.substr(0, separator), path.substr(separator + 1)};
}

const ScenarioSchema&
GetScenarioSchema()
{
    static const ScenarioSchema schema = [] {
        ScenarioSchema result;
        for (const auto& option : GetScenarioConfigOptions())
        {
            const auto [section, field] = SplitTomlPath(option.tomlPath);
            result[std::string(section)].insert(std::string(field));
        }
        return result;
    }();
    return schema;
}

void
ValidateSchema(const toml::table& document)
{
    const auto& schema = GetScenarioSchema();
    for (const auto& [sectionKey, sectionNode] : document)
    {
        const std::string section(sectionKey.str());
        const auto schemaSection = schema.find(section);
        if (schemaSection == schema.end())
        {
            throw ScenarioConfigError("unknown TOML section '" + section + "'");
        }

        const auto* table = sectionNode.as_table();
        if (!table)
        {
            throw ScenarioConfigError("invalid TOML section '" + section + "': expected table");
        }
        for (const auto& [fieldKey, fieldNode] : *table)
        {
            static_cast<void>(fieldNode);
            const std::string field(fieldKey.str());
            if (!schemaSection->second.contains(field))
            {
                throw ScenarioConfigError("unknown TOML field '" + section + "." + field + "'");
            }
        }
    }
}

std::string
FormatParseError(const std::filesystem::path& requestedPath, const toml::parse_error& error)
{
    const auto& source = error.source();
    const std::string sourcePath = source.path ? *source.path : requestedPath.string();
    std::ostringstream message;
    message << sourcePath << ':' << source.begin.line << ':' << source.begin.column << ": "
            << error.description();
    return message.str();
}

} // namespace

ScenarioConfig
LoadTomlConfig(const std::filesystem::path& path)
{
    toml::table document;
    try
    {
        document = toml::parse_file(path.string());
    }
    catch (const toml::parse_error& error)
    {
        throw ScenarioConfigError(FormatParseError(path, error));
    }

    ValidateSchema(document);

    ScenarioConfig config;
    for (const auto& option : GetScenarioConfigOptions())
    {
        const auto [section, field] = SplitTomlPath(option.tomlPath);
        const auto* table = document.get_as<toml::table>(section);
        if (table)
        {
            const auto* node = table->get(field);
            if (node)
            {
                option.applyToml(config, *node);
            }
        }
    }

    const auto* general = document.get_as<toml::table>("general");
    if (!general || !general->contains("trace_file"))
    {
        throw ScenarioConfigError("missing required general.trace_file string");
    }
    if (config.general.traceFile.empty())
    {
        throw ScenarioConfigError("invalid general.trace_file: expected non-empty string");
    }
    return config;
}

} // namespace ns3
