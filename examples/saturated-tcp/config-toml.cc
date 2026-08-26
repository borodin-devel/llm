#include "config-internal.h"

#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>

namespace ns3
{

namespace
{

using SaturatedSchema = std::map<std::string, std::set<std::string>>;

const SaturatedSchema&
GetSaturatedSchema()
{
    static const SaturatedSchema schema = [] {
        SaturatedSchema result;
        for (const auto& option : GetSaturatedTcpConfigOptions())
        {
            const auto [section, field] = SplitSaturatedTcpConfigPath(option.tomlPath);
            result[std::string(section)].insert(std::string(field));
        }
        return result;
    }();
    return schema;
}

void
ValidateSchema(const toml::table& document)
{
    const auto& schema = GetSaturatedSchema();
    for (const auto& [sectionKey, sectionNode] : document)
    {
        const std::string section(sectionKey.str());
        const auto schemaSection = schema.find(section);
        if (schemaSection == schema.end())
        {
            throw SaturatedTcpConfigError("unknown saturated TOML section '" + section + "'");
        }

        const auto* table = sectionNode.as_table();
        if (!table)
        {
            throw SaturatedTcpConfigError("invalid saturated TOML section '" + section +
                                          "': expected table");
        }
        for (const auto& [fieldKey, fieldNode] : *table)
        {
            static_cast<void>(fieldNode);
            const std::string field(fieldKey.str());
            if (!schemaSection->second.contains(field))
            {
                throw SaturatedTcpConfigError("unknown saturated TOML field '" + section + "." +
                                              field + "'");
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
    message << "invalid saturated TOML " << sourcePath << ':' << source.begin.line << ':'
            << source.begin.column << ": " << error.description();
    return message.str();
}

} // namespace

std::pair<std::string_view, std::string_view>
SplitSaturatedTcpConfigPath(std::string_view path)
{
    const auto separator = path.find('.');
    if (separator == std::string_view::npos || separator == 0 || separator == path.size() - 1 ||
        path.find('.', separator + 1) != std::string_view::npos)
    {
        throw SaturatedTcpConfigError("invalid saturated configuration path: " + std::string(path));
    }
    return {path.substr(0, separator), path.substr(separator + 1)};
}

SaturatedTcpConfig
LoadSaturatedTcpTomlConfig(const std::filesystem::path& path)
{
    toml::table document;
    try
    {
        document = toml::parse_file(path.string());
    }
    catch (const toml::parse_error& error)
    {
        throw SaturatedTcpConfigError(FormatParseError(path, error));
    }

    ValidateSchema(document);

    SaturatedTcpConfig config;
    for (const auto& option : GetSaturatedTcpConfigOptions())
    {
        const auto [section, field] = SplitSaturatedTcpConfigPath(option.tomlPath);
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
    return config;
}

} // namespace ns3
