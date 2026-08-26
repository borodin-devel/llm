#include "config-internal.h"

#include <algorithm>
#include <filesystem>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace ns3
{

namespace
{

/** One deferred command-line override. */
struct SaturatedRawOverride
{
    const SaturatedConfigOption* option; ///< Registry option to update.
    std::string value;                   ///< Unparsed command-line value.
};

const SaturatedConfigOption*
FindOption(std::string_view flag)
{
    const auto& options = GetSaturatedTcpConfigOptions();
    const auto option = std::find_if(options.begin(), options.end(), [flag](const auto& candidate) {
        return candidate.cliFlag == flag;
    });
    return option == options.end() ? nullptr : &*option;
}

std::pair<std::string, std::optional<std::string>>
SplitArgument(std::string_view argument)
{
    const auto separator = argument.find('=');
    if (separator == std::string_view::npos)
    {
        return {std::string(argument), std::nullopt};
    }
    return {std::string(argument.substr(0, separator)),
            std::string(argument.substr(separator + 1))};
}

} // namespace

SaturatedTcpConfig
ParseSaturatedTcpConfig(int argc, char** argv)
{
    if (argc < 1 || argv == nullptr || argv[0] == nullptr)
    {
        throw SaturatedTcpConfigError("invalid saturated argument vector");
    }

    std::set<std::string> seenFlags;
    std::vector<SaturatedRawOverride> overrides;
    std::filesystem::path configPath;
    bool hasConfig = false;

    for (int index = 1; index < argc; ++index)
    {
        if (argv[index] == nullptr)
        {
            throw SaturatedTcpConfigError("invalid saturated null command-line argument");
        }
        auto [flag, attachedValue] = SplitArgument(argv[index]);
        if (!flag.starts_with("--"))
        {
            throw SaturatedTcpConfigError("unexpected saturated positional argument: " + flag);
        }

        const SaturatedConfigOption* option = FindOption(flag);
        if (flag != "--config" && !option)
        {
            throw SaturatedTcpConfigError("unknown saturated flag: " + flag);
        }
        if (!seenFlags.insert(flag).second)
        {
            throw SaturatedTcpConfigError("duplicate saturated flag: " + flag);
        }

        std::string value;
        if (attachedValue)
        {
            value = std::move(*attachedValue);
        }
        else
        {
            if (index + 1 == argc || argv[index + 1] == nullptr)
            {
                throw SaturatedTcpConfigError("saturated flag " + flag + " requires a value");
            }
            const std::string_view candidateValue(argv[index + 1]);
            if (candidateValue.starts_with("--"))
            {
                const auto [candidateFlag, candidateAttachedValue] = SplitArgument(candidateValue);
                static_cast<void>(candidateAttachedValue);
                if (candidateFlag != "--config" && !FindOption(candidateFlag))
                {
                    throw SaturatedTcpConfigError("unknown saturated flag: " + candidateFlag);
                }
                throw SaturatedTcpConfigError("saturated flag " + flag + " requires a value");
            }
            ++index;
            value = argv[index];
        }
        if (value.empty())
        {
            throw SaturatedTcpConfigError("saturated flag " + flag + " requires a value");
        }

        if (flag == "--config")
        {
            configPath = value;
            hasConfig = true;
        }
        else
        {
            overrides.push_back({option, std::move(value)});
        }
    }

    if (!hasConfig)
    {
        throw SaturatedTcpConfigError("exactly one saturated --config <path> is required");
    }
    std::error_code errorCode;
    if (!std::filesystem::is_regular_file(configPath, errorCode))
    {
        throw SaturatedTcpConfigError("saturated config path is not a regular file: " +
                                      configPath.string());
    }

    SaturatedTcpConfig config = LoadSaturatedTcpTomlConfig(configPath);
    for (const auto& override : overrides)
    {
        try
        {
            override.option->applyOverride(config, override.value);
        }
        catch (const SaturatedTcpConfigError& error)
        {
            throw SaturatedTcpConfigError("invalid saturated value for " +
                                          override.option->cliFlag + " (TOML " +
                                          override.option->tomlPath + "): " + error.what());
        }
    }
    ValidateSaturatedTcpConfig(config);
    return config;
}

} // namespace ns3
