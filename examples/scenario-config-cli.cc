#include "scenario-config-internal.h"
#include "scenario-config.h"

#include <algorithm>
#include <filesystem>
#include <iomanip>
#include <ostream>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace ns3
{

namespace
{

struct RawOverride
{
    const ConfigOption* option;
    std::string value;
};

ScenarioCommandLineResult
MakeError(std::string message, bool printUsage = false)
{
    ScenarioCommandLineResult result;
    result.printUsage = printUsage;
    result.error = std::move(message);
    return result;
}

const ConfigOption*
FindOption(std::string_view flag)
{
    const auto& options = GetScenarioConfigOptions();
    const auto option = std::find_if(options.begin(), options.end(), [flag](const auto& candidate) {
        return candidate.cliFlag == flag;
    });
    return option == options.end() ? nullptr : &*option;
}

std::filesystem::path
ResolveConfigFile(const std::filesystem::path& configuredPath,
                  const std::filesystem::path& workingDirectory)
{
    if (configuredPath.is_absolute())
    {
        return configuredPath.lexically_normal();
    }
    return (workingDirectory / configuredPath).lexically_normal();
}

} // namespace

ScenarioCommandLineResult
ParseScenarioArguments(const std::vector<std::string>& arguments,
                       const std::filesystem::path& workingDirectory)
{
    std::set<std::string> seenFlags;
    std::vector<RawOverride> overrides;
    std::filesystem::path configuredPath;
    bool helpRequested = false;
    bool hasConfig = false;

    for (std::size_t index = 0; index < arguments.size(); ++index)
    {
        const std::string& flag = arguments[index];
        if (flag == "--help")
        {
            if (!seenFlags.insert(flag).second)
            {
                return MakeError("duplicate --help");
            }
            helpRequested = true;
            continue;
        }

        const ConfigOption* option = FindOption(flag);
        if (flag != "--config" && !option)
        {
            if (flag.starts_with("--"))
            {
                return MakeError("unknown flag: " + flag);
            }
            return MakeError("unexpected positional argument: " + flag);
        }
        if (!seenFlags.insert(flag).second)
        {
            return MakeError("duplicate " + flag);
        }
        if (++index == arguments.size())
        {
            return MakeError(flag + " requires a value");
        }

        if (flag == "--config")
        {
            configuredPath = arguments[index];
            hasConfig = true;
        }
        else
        {
            overrides.push_back({option, arguments[index]});
        }
    }

    if (helpRequested)
    {
        ScenarioCommandLineResult result;
        result.valid = true;
        result.printUsage = true;
        return result;
    }
    if (!hasConfig)
    {
        return MakeError("exactly one --config <path> is required", true);
    }

    const auto configFile = ResolveConfigFile(configuredPath, workingDirectory);
    std::error_code errorCode;
    if (!std::filesystem::is_regular_file(configFile, errorCode))
    {
        return MakeError("config path is not a regular file: " + configFile.string());
    }

    ScenarioConfig scenario;
    try
    {
        scenario = LoadTomlConfig(configFile);
        for (const auto& override : overrides)
        {
            try
            {
                override.option->applyOverride(scenario, override.value);
            }
            catch (const ScenarioConfigError& error)
            {
                throw ScenarioConfigError("invalid value for " + override.option->cliFlag +
                                          " (TOML " + override.option->tomlPath +
                                          "): " + error.what());
            }
        }
    }
    catch (const ScenarioConfigError& error)
    {
        return MakeError(error.what());
    }

    ScenarioCommandLineResult result;
    result.valid = true;
    result.launch.scenario = std::move(scenario);
    result.launch.configFile = configFile;
    result.launch.workingDirectory = workingDirectory;
    return result;
}

void
PrintScenarioUsage(std::ostream& output, const std::string& programName)
{
    std::vector<std::pair<std::string, const ConfigOption*>> optionLines;
    std::size_t optionWidth = std::string("--config <path>").size();
    for (const auto& option : GetScenarioConfigOptions())
    {
        std::string signature = option.cliFlag + " <" +
                                std::string(GetScenarioConfigValueTypeName(option.valueType)) + ">";
        optionWidth = std::max(optionWidth, signature.size());
        optionLines.emplace_back(std::move(signature), &option);
    }
    optionWidth += 2;

    output << "Usage: " << programName
           << " --config <config.toml> [--section-field <value> ...]\n\nOptions:\n"
           << "  " << std::left << std::setw(optionWidth) << "--help" << "Show this help\n"
           << "  " << std::left << std::setw(optionWidth) << "--config <path>"
           << "TOML configuration file\n";
    for (const auto& [signature, option] : optionLines)
    {
        output << "  " << std::left << std::setw(optionWidth) << signature << option->description
               << " (TOML: " << option->tomlPath << ")\n";
    }
}

} // namespace ns3
