#include "scenario-config.h"

#include "scenario-config-internal.h"

#include <charconv>
#include <cmath>
#include <exception>
#include <ostream>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace ns3
{

namespace
{

std::string
DeriveCliFlag(std::string_view tomlPath)
{
    std::string flag{"--"};
    flag.append(tomlPath);
    for (char& character : flag)
    {
        if (character == '.' || character == '_')
        {
            character = '-';
        }
    }
    return flag;
}

std::string_view
ValueTypeName(ConfigValueType valueType)
{
    switch (valueType)
    {
    case ConfigValueType::STRING:
        return "string";
    case ConfigValueType::INTEGER:
        return "integer";
    case ConfigValueType::FLOAT:
        return "floating-point number";
    case ConfigValueType::BOOLEAN:
        return "Boolean";
    case ConfigValueType::ENUM:
        return "enumerated string";
    }
    return "scalar";
}

[[noreturn]] void
ThrowExpected(std::string_view tomlPath, ConfigValueType valueType)
{
    throw ScenarioConfigError("invalid " + std::string(tomlPath) + ": expected " +
                              std::string(ValueTypeName(valueType)));
}

template <typename T>
using ConfigAccessor = std::function<T&(ScenarioConfig&)>;

template <typename T>
ConfigOption
MakeIntegerOption(std::string_view tomlPath,
                  ConfigAccessor<T> accessor,
                  std::string_view description)
{
    static_assert(std::is_integral_v<T> && !std::is_same_v<T, bool>);
    ConfigOption option;
    option.tomlPath = tomlPath;
    option.cliFlag = DeriveCliFlag(tomlPath);
    option.valueType = ConfigValueType::INTEGER;
    option.applyToml = [path = option.tomlPath, accessor](ScenarioConfig& config,
                                                          const toml::node& node) {
        const auto* value = node.as_integer();
        if (!value || !std::in_range<T>(value->get()))
        {
            ThrowExpected(path, ConfigValueType::INTEGER);
        }
        accessor(config) = static_cast<T>(value->get());
    };
    option.applyOverride = [path = option.tomlPath, accessor](ScenarioConfig& config,
                                                              std::string_view text) {
        if constexpr (std::is_signed_v<T>)
        {
            int64_t value{};
            const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
            if (result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
                !std::in_range<T>(value))
            {
                ThrowExpected(path, ConfigValueType::INTEGER);
            }
            accessor(config) = static_cast<T>(value);
        }
        else
        {
            uint64_t value{};
            const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
            if (result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
                !std::in_range<T>(value))
            {
                ThrowExpected(path, ConfigValueType::INTEGER);
            }
            accessor(config) = static_cast<T>(value);
        }
    };
    option.description = description;
    return option;
}

ConfigOption
MakeFloatOption(std::string_view tomlPath,
                ConfigAccessor<double> accessor,
                std::string_view description)
{
    ConfigOption option;
    option.tomlPath = tomlPath;
    option.cliFlag = DeriveCliFlag(tomlPath);
    option.valueType = ConfigValueType::FLOAT;
    option.applyToml = [path = option.tomlPath, accessor](ScenarioConfig& config,
                                                          const toml::node& node) {
        const auto* value = node.as_floating_point();
        if (!value)
        {
            ThrowExpected(path, ConfigValueType::FLOAT);
        }
        accessor(config) = value->get();
    };
    option.applyOverride = [path = option.tomlPath, accessor](ScenarioConfig& config,
                                                              std::string_view text) {
        double value{};
        const auto result = std::from_chars(text.data(),
                                            text.data() + text.size(),
                                            value,
                                            std::chars_format::general);
        if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
        {
            ThrowExpected(path, ConfigValueType::FLOAT);
        }
        accessor(config) = value;
    };
    option.description = description;
    return option;
}

ConfigOption
MakeBooleanOption(std::string_view tomlPath,
                  ConfigAccessor<bool> accessor,
                  std::string_view description)
{
    ConfigOption option;
    option.tomlPath = tomlPath;
    option.cliFlag = DeriveCliFlag(tomlPath);
    option.valueType = ConfigValueType::BOOLEAN;
    option.applyToml = [path = option.tomlPath, accessor](ScenarioConfig& config,
                                                          const toml::node& node) {
        const auto* value = node.as_boolean();
        if (!value)
        {
            ThrowExpected(path, ConfigValueType::BOOLEAN);
        }
        accessor(config) = value->get();
    };
    option.applyOverride = [path = option.tomlPath, accessor](ScenarioConfig& config,
                                                              std::string_view text) {
        if (text == "true")
        {
            accessor(config) = true;
        }
        else if (text == "false")
        {
            accessor(config) = false;
        }
        else
        {
            ThrowExpected(path, ConfigValueType::BOOLEAN);
        }
    };
    option.description = description;
    return option;
}

ConfigOption
MakeStringOption(std::string_view tomlPath,
                 ConfigAccessor<std::string> accessor,
                 std::string_view description)
{
    ConfigOption option;
    option.tomlPath = tomlPath;
    option.cliFlag = DeriveCliFlag(tomlPath);
    option.valueType = ConfigValueType::STRING;
    option.applyToml = [path = option.tomlPath, accessor](ScenarioConfig& config,
                                                          const toml::node& node) {
        const auto* value = node.as_string();
        if (!value)
        {
            ThrowExpected(path, ConfigValueType::STRING);
        }
        accessor(config) = value->get();
    };
    option.applyOverride = [accessor](ScenarioConfig& config, std::string_view text) {
        accessor(config) = text;
    };
    option.description = description;
    return option;
}

ConfigOption
MakeOptionalStringOption(std::string_view tomlPath,
                         ConfigAccessor<std::optional<std::string>> accessor,
                         std::string_view description)
{
    ConfigOption option;
    option.tomlPath = tomlPath;
    option.cliFlag = DeriveCliFlag(tomlPath);
    option.valueType = ConfigValueType::STRING;
    option.applyToml = [path = option.tomlPath, accessor](ScenarioConfig& config,
                                                          const toml::node& node) {
        const auto* value = node.as_string();
        if (!value)
        {
            ThrowExpected(path, ConfigValueType::STRING);
        }
        accessor(config) = value->get();
    };
    option.applyOverride = [accessor](ScenarioConfig& config, std::string_view text) {
        accessor(config) = std::string(text);
    };
    option.description = description;
    return option;
}

template <typename T>
ConfigOption
MakeEnumOption(std::string_view tomlPath,
               ConfigAccessor<T> accessor,
               std::vector<std::pair<std::string_view, T>> values,
               std::string_view description)
{
    ConfigOption option;
    option.tomlPath = tomlPath;
    option.cliFlag = DeriveCliFlag(tomlPath);
    option.valueType = ConfigValueType::ENUM;
    const auto apply = [path = option.tomlPath,
                        accessor,
                        values = std::move(values)](ScenarioConfig& config, std::string_view text) {
        for (const auto& [name, value] : values)
        {
            if (text == name)
            {
                accessor(config) = value;
                return;
            }
        }
        ThrowExpected(path, ConfigValueType::ENUM);
    };
    option.applyToml = [path = option.tomlPath, apply](ScenarioConfig& config,
                                                       const toml::node& node) {
        const auto* value = node.as_string();
        if (!value)
        {
            ThrowExpected(path, ConfigValueType::ENUM);
        }
        apply(config, value->get());
    };
    option.applyOverride = apply;
    option.description = description;
    return option;
}

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

ScenarioConfigError::ScenarioConfigError(const std::string& message)
    : std::runtime_error(message)
{
}

const std::vector<ConfigOption>&
GetScenarioConfigOptions()
{
    static const std::vector<ConfigOption> options{
        MakeStringOption(
            "general.trace_file",
            [](auto& c) -> auto& { return c.general.traceFile; },
            "Input JSON trace path"),
        MakeOptionalStringOption(
            "general.run_folder",
            [](auto& c) -> auto& { return c.general.runFolder; },
            "Optional exact output directory"),
        MakeStringOption(
            "general.output_name",
            [](auto& c) -> auto& { return c.general.outputName; },
            "Statistics JSON filename"),
        MakeEnumOption<DurationMode>(
            "simulation.duration_mode",
            [](auto& c) -> auto& { return c.simulation.durationMode; },
            {{"auto", DurationMode::AUTO}, {"fixed", DurationMode::FIXED}},
            "Experiment duration policy"),
        MakeFloatOption(
            "simulation.fixed_duration_seconds",
            [](auto& c) -> auto& { return c.simulation.fixedDurationSeconds; },
            "Fixed experiment duration in seconds"),
        MakeFloatOption(
            "simulation.auto_tail_seconds",
            [](auto& c) -> auto& { return c.simulation.autoTailSeconds; },
            "Automatic-duration tail in seconds"),
        MakeIntegerOption<uint32_t>(
            "simulation.rng_seed",
            [](auto& c) -> auto& { return c.simulation.rngSeed; },
            "ns-3 random-number seed"),
        MakeIntegerOption<uint64_t>(
            "simulation.rng_run",
            [](auto& c) -> auto& { return c.simulation.rngRun; },
            "ns-3 run/substream number"),
        MakeIntegerOption<int>(
            "topology.bss_count",
            [](auto& c) -> auto& { return c.topology.bssCount; },
            "Number of AP/BSS groups"),
        MakeIntegerOption<int>(
            "topology.stations_per_bss",
            [](auto& c) -> auto& { return c.topology.stationsPerBss; },
            "Physical stations per BSS"),
        MakeFloatOption(
            "topology.bss_spacing_m",
            [](auto& c) -> auto& { return c.topology.bssSpacingM; },
            "AP spacing in meters"),
        MakeFloatOption(
            "topology.station_radius_m",
            [](auto& c) -> auto& { return c.topology.stationRadiusM; },
            "Station-disc radius in meters"),
        MakeBooleanOption(
            "topology.isolate_bss_channels",
            [](auto& c) -> auto& { return c.topology.isolateBssChannels; },
            "Whether BSS groups use separate channels"),
        MakeStringOption(
            "topology.ssid_prefix",
            [](auto& c) -> auto& { return c.topology.ssidPrefix; },
            "Prefix used to build BSS SSIDs"),
        MakeIntegerOption<uint16_t>(
            "topology.ap_sink_port",
            [](auto& c) -> auto& { return c.topology.apSinkPort; },
            "TCP sink port on each AP"),
        MakeIntegerOption<uint16_t>(
            "topology.station_sink_base_port",
            [](auto& c) -> auto& { return c.topology.stationSinkBasePort; },
            "First TCP station sink port"),
        MakeFloatOption(
            "topology.generator_start_seconds",
            [](auto& c) -> auto& { return c.topology.generatorStartSeconds; },
            "Generator start time in seconds"),
        MakeIntegerOption<int>(
            "distribution.max_agents_per_station",
            [](auto& c) -> auto& { return c.distribution.maxAgentsPerStation; },
            "Agent cap per physical station"),
        MakeBooleanOption(
            "distribution.low_contention_priority",
            [](auto& c) -> auto& { return c.distribution.lowContentionPriority; },
            "Whether to minimize contention first"),
        MakeIntegerOption<int>(
            "distribution.slot_ms",
            [](auto& c) -> auto& { return c.distribution.slotMs; },
            "Uplink-overlap slot width in milliseconds"),
        MakeEnumOption<WifiBandConfig>(
            "wifi.band",
            [](auto& c) -> auto& { return c.wifi.band; },
            {{"2.4GHz", WifiBandConfig::BAND_2_4_GHZ},
             {"5GHz", WifiBandConfig::BAND_5_GHZ},
             {"6GHz", WifiBandConfig::BAND_6_GHZ}},
            "Fixed 802.11ax operating band"),
        MakeIntegerOption<uint16_t>(
            "wifi.channel_number",
            [](auto& c) -> auto& { return c.wifi.channelNumber; },
            "IEEE channel number"),
        MakeIntegerOption<int>(
            "wifi.bandwidth_mhz",
            [](auto& c) -> auto& { return c.wifi.bandwidthMhz; },
            "Channel width in MHz"),
        MakeIntegerOption<uint8_t>(
            "wifi.primary_20_index",
            [](auto& c) -> auto& { return c.wifi.primary20Index; },
            "Primary 20 MHz subchannel index"),
        MakeStringOption(
            "wifi.rate_manager",
            [](auto& c) -> auto& { return c.wifi.rateManager; },
            "Wi-Fi rate-manager TypeId name"),
        MakeBooleanOption(
            "wifi.active_probing",
            [](auto& c) -> auto& { return c.wifi.activeProbing; },
            "Whether stations actively probe"),
        MakeStringOption(
            "tcp.congestion_control",
            [](auto& c) -> auto& { return c.tcp.congestionControl; },
            "TCP congestion-control TypeId name"),
        MakeIntegerOption<uint32_t>(
            "tcp.segment_size_bytes",
            [](auto& c) -> auto& { return c.tcp.segmentSizeBytes; },
            "TCP maximum segment payload in bytes"),
        MakeIntegerOption<uint32_t>(
            "tcp.send_buffer_bytes",
            [](auto& c) -> auto& { return c.tcp.sendBufferBytes; },
            "TCP send-buffer size in bytes"),
        MakeIntegerOption<uint32_t>(
            "tcp.receive_buffer_bytes",
            [](auto& c) -> auto& { return c.tcp.receiveBufferBytes; },
            "TCP receive-buffer size in bytes"),
        MakeIntegerOption<uint32_t>(
            "statistics.window_ms",
            [](auto& c) -> auto& { return c.statistics.windowMs; },
            "Sparse PHY statistics window in milliseconds"),
        MakeStringOption(
            "logging.sample_scenario_level",
            [](auto& c) -> auto& { return c.logging.sampleScenarioLevel; },
            "SampleScenario log level"),
        MakeStringOption(
            "logging.ap_generator_level",
            [](auto& c) -> auto& { return c.logging.apGeneratorLevel; },
            "APGenerator log level"),
        MakeStringOption(
            "logging.sta_generator_level",
            [](auto& c) -> auto& { return c.logging.staGeneratorLevel; },
            "StaLlmGenerator log level"),
        MakeStringOption(
            "logging.traffic_sink_level",
            [](auto& c) -> auto& { return c.logging.trafficSinkLevel; },
            "TrafficSink log level"),
        MakeStringOption(
            "logging.contention_distribution_level",
            [](auto& c) -> auto& { return c.logging.contentionDistributionLevel; },
            "Contention distribution log level")};
    return options;
}

const std::vector<ConfigOptionInfo>&
GetScenarioConfigOptionInfo()
{
    static const std::vector<ConfigOptionInfo> info = [] {
        std::vector<ConfigOptionInfo> result;
        result.reserve(GetScenarioConfigOptions().size());
        for (const auto& option : GetScenarioConfigOptions())
        {
            result.push_back(
                {option.tomlPath, option.cliFlag, option.valueType, option.description});
        }
        return result;
    }();
    return info;
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
