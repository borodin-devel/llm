#include "config-internal.h"

#include <charconv>
#include <ostream>
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
ValueTypeName(SaturatedConfigValueType valueType)
{
    switch (valueType)
    {
    case SaturatedConfigValueType::STRING:
        return "string";
    case SaturatedConfigValueType::INTEGER:
        return "integer";
    case SaturatedConfigValueType::FLOAT:
        return "floating-point number";
    case SaturatedConfigValueType::ENUM:
        return "enumerated string";
    }
    return "scalar";
}

[[noreturn]] void
ThrowExpected(std::string_view tomlPath, SaturatedConfigValueType valueType)
{
    throw SaturatedTcpConfigError("invalid saturated " + std::string(tomlPath) + ": expected " +
                                  std::string(ValueTypeName(valueType)));
}

template <typename T, typename Accessor>
SaturatedConfigOption
MakeIntegerOption(std::string_view tomlPath, Accessor accessor, std::string_view description)
{
    static_assert(std::is_integral_v<T> && !std::is_same_v<T, bool>);
    SaturatedConfigOption option;
    option.tomlPath = tomlPath;
    option.cliFlag = DeriveCliFlag(tomlPath);
    option.valueType = SaturatedConfigValueType::INTEGER;
    option.applyToml = [path = option.tomlPath, accessor](SaturatedTcpConfig& config,
                                                          const toml::node& node) {
        const auto* value = node.as_integer();
        if (!value || !std::in_range<T>(value->get()))
        {
            ThrowExpected(path, SaturatedConfigValueType::INTEGER);
        }
        accessor(config) = static_cast<T>(value->get());
    };
    option.applyOverride = [path = option.tomlPath, accessor](SaturatedTcpConfig& config,
                                                              std::string_view text) {
        uint64_t value{};
        const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
        if (result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
            !std::in_range<T>(value))
        {
            ThrowExpected(path, SaturatedConfigValueType::INTEGER);
        }
        accessor(config) = static_cast<T>(value);
    };
    option.readJson = [accessor](const SaturatedTcpConfig& config) {
        return nlohmann::json(accessor(config));
    };
    option.description = description;
    return option;
}

template <typename Accessor>
SaturatedConfigOption
MakeFloatOption(std::string_view tomlPath, Accessor accessor, std::string_view description)
{
    SaturatedConfigOption option;
    option.tomlPath = tomlPath;
    option.cliFlag = DeriveCliFlag(tomlPath);
    option.valueType = SaturatedConfigValueType::FLOAT;
    option.applyToml = [path = option.tomlPath, accessor](SaturatedTcpConfig& config,
                                                          const toml::node& node) {
        const auto* value = node.as_floating_point();
        if (!value)
        {
            ThrowExpected(path, SaturatedConfigValueType::FLOAT);
        }
        accessor(config) = value->get();
    };
    option.applyOverride = [path = option.tomlPath, accessor](SaturatedTcpConfig& config,
                                                              std::string_view text) {
        double value{};
        const auto result = std::from_chars(text.data(),
                                            text.data() + text.size(),
                                            value,
                                            std::chars_format::general);
        if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
        {
            ThrowExpected(path, SaturatedConfigValueType::FLOAT);
        }
        accessor(config) = value;
    };
    option.readJson = [accessor](const SaturatedTcpConfig& config) {
        return nlohmann::json(accessor(config));
    };
    option.description = description;
    return option;
}

template <typename Accessor>
SaturatedConfigOption
MakeStringOption(std::string_view tomlPath, Accessor accessor, std::string_view description)
{
    SaturatedConfigOption option;
    option.tomlPath = tomlPath;
    option.cliFlag = DeriveCliFlag(tomlPath);
    option.valueType = SaturatedConfigValueType::STRING;
    option.applyToml = [path = option.tomlPath, accessor](SaturatedTcpConfig& config,
                                                          const toml::node& node) {
        const auto* value = node.as_string();
        if (!value)
        {
            ThrowExpected(path, SaturatedConfigValueType::STRING);
        }
        accessor(config) = value->get();
    };
    option.applyOverride = [accessor](SaturatedTcpConfig& config, std::string_view text) {
        accessor(config) = text;
    };
    option.readJson = [accessor](const SaturatedTcpConfig& config) {
        return nlohmann::json(accessor(config));
    };
    option.description = description;
    return option;
}

template <typename Accessor>
SaturatedConfigOption
MakeOptionalStringOption(std::string_view tomlPath, Accessor accessor, std::string_view description)
{
    SaturatedConfigOption option;
    option.tomlPath = tomlPath;
    option.cliFlag = DeriveCliFlag(tomlPath);
    option.valueType = SaturatedConfigValueType::STRING;
    option.applyToml = [path = option.tomlPath, accessor](SaturatedTcpConfig& config,
                                                          const toml::node& node) {
        const auto* value = node.as_string();
        if (!value)
        {
            ThrowExpected(path, SaturatedConfigValueType::STRING);
        }
        accessor(config) = value->get();
    };
    option.applyOverride = [accessor](SaturatedTcpConfig& config, std::string_view text) {
        accessor(config) = std::string(text);
    };
    option.readJson = [accessor](const SaturatedTcpConfig& config) {
        const auto& value = accessor(config);
        return value ? nlohmann::json(*value) : nlohmann::json(nullptr);
    };
    option.description = description;
    return option;
}

template <typename T, typename Accessor>
SaturatedConfigOption
MakeEnumOption(std::string_view tomlPath,
               Accessor accessor,
               std::vector<std::pair<std::string_view, T>> values,
               std::string_view description)
{
    SaturatedConfigOption option;
    option.tomlPath = tomlPath;
    option.cliFlag = DeriveCliFlag(tomlPath);
    option.valueType = SaturatedConfigValueType::ENUM;
    auto readerValues = values;
    const auto apply =
        [path = option.tomlPath, accessor, values = std::move(values)](SaturatedTcpConfig& config,
                                                                       std::string_view text) {
            for (const auto& [name, value] : values)
            {
                if (text == name)
                {
                    accessor(config) = value;
                    return;
                }
            }
            ThrowExpected(path, SaturatedConfigValueType::ENUM);
        };
    option.applyToml = [path = option.tomlPath, apply](SaturatedTcpConfig& config,
                                                       const toml::node& node) {
        const auto* value = node.as_string();
        if (!value)
        {
            ThrowExpected(path, SaturatedConfigValueType::ENUM);
        }
        apply(config, value->get());
    };
    option.applyOverride = apply;
    option.readJson = [path = option.tomlPath, accessor, values = std::move(readerValues)](
                          const SaturatedTcpConfig& config) {
        for (const auto& [name, value] : values)
        {
            if (accessor(config) == value)
            {
                return nlohmann::json(name);
            }
        }
        throw SaturatedTcpConfigError("invalid saturated " + path + ": no canonical enum spelling");
    };
    option.description = description;
    return option;
}

} // namespace

std::ostream&
operator<<(std::ostream& output, SaturatedRssiRange value)
{
    switch (value)
    {
    case SaturatedRssiRange::HIGH:
        return output << "high";
    case SaturatedRssiRange::MEDIUM:
        return output << "medium";
    case SaturatedRssiRange::LOW:
        return output << "low";
    }
    return output << "unknown saturated RSSI range";
}

std::ostream&
operator<<(std::ostream& output, SaturatedInterferenceMode value)
{
    switch (value)
    {
    case SaturatedInterferenceMode::ISOLATED:
        return output << "isolated";
    case SaturatedInterferenceMode::AP_ONLY_COCHANNEL:
        return output << "ap_only_cochannel";
    }
    return output << "unknown saturated interference mode";
}

std::ostream&
operator<<(std::ostream& output, SaturatedTrafficMode value)
{
    switch (value)
    {
    case SaturatedTrafficMode::UL:
        return output << "ul";
    case SaturatedTrafficMode::DL:
        return output << "dl";
    case SaturatedTrafficMode::UL_DL:
        return output << "ul_dl";
    }
    return output << "unknown saturated traffic mode";
}

std::ostream&
operator<<(std::ostream& output, SaturatedMimoMode value)
{
    switch (value)
    {
    case SaturatedMimoMode::SU:
        return output << "su";
    case SaturatedMimoMode::MU:
        return output << "mu";
    }
    return output << "unknown saturated MIMO mode";
}

SaturatedTcpConfigError::SaturatedTcpConfigError(const std::string& message)
    : std::runtime_error(message)
{
}

std::string_view
GetSaturatedTcpConfigValueTypeName(SaturatedConfigValueType valueType)
{
    return ValueTypeName(valueType);
}

const std::vector<SaturatedConfigOption>&
GetSaturatedTcpConfigOptions()
{
    static const std::vector<SaturatedConfigOption> options{
        MakeStringOption(
            "general.output_name",
            [](auto& c) -> auto& { return c.general.outputName; },
            "Statistics JSON filename"),
        MakeOptionalStringOption(
            "general.run_folder",
            [](auto& c) -> auto& { return c.general.runFolder; },
            "Optional exact output directory"),
        MakeIntegerOption<uint32_t>(
            "script.repetitions",
            [](auto& c) -> auto& { return c.script.repetitions; },
            "External runner repetition metadata"),
        MakeIntegerOption<uint32_t>(
            "simulation.rng_seed",
            [](auto& c) -> auto& { return c.simulation.rngSeed; },
            "ns-3 random-number seed"),
        MakeIntegerOption<uint64_t>(
            "simulation.rng_run",
            [](auto& c) -> auto& { return c.simulation.rngRun; },
            "ns-3 random-number run number"),
        MakeIntegerOption<uint32_t>(
            "benchmark.sta_count_per_bss",
            [](auto& c) -> auto& { return c.benchmark.stationCountPerBss; },
            "Stations in each BSS"),
        MakeEnumOption<SaturatedRssiRange>(
            "benchmark.rssi_range",
            [](auto& c) -> auto& { return c.benchmark.rssiRange; },
            {{"high", SaturatedRssiRange::HIGH},
             {"medium", SaturatedRssiRange::MEDIUM},
             {"low", SaturatedRssiRange::LOW}},
            "Target RSSI range"),
        MakeEnumOption<SaturatedInterferenceMode>(
            "benchmark.interference_mode",
            [](auto& c) -> auto& { return c.benchmark.interferenceMode; },
            {{"isolated", SaturatedInterferenceMode::ISOLATED},
             {"ap_only_cochannel", SaturatedInterferenceMode::AP_ONLY_COCHANNEL}},
            "Inter-BSS interference mode"),
        MakeEnumOption<SaturatedTrafficMode>(
            "benchmark.traffic_mode",
            [](auto& c) -> auto& { return c.benchmark.trafficMode; },
            {{"ul", SaturatedTrafficMode::UL},
             {"dl", SaturatedTrafficMode::DL},
             {"ul_dl", SaturatedTrafficMode::UL_DL}},
            "Saturated traffic direction"),
        MakeEnumOption<SaturatedMimoMode>(
            "benchmark.mimo_mode",
            [](auto& c) -> auto& { return c.benchmark.mimoMode; },
            {{"su", SaturatedMimoMode::SU}, {"mu", SaturatedMimoMode::MU}},
            "Spatial transmission mode"),
        MakeIntegerOption<uint32_t>(
            "benchmark.traffic_warmup_seconds",
            [](auto& c) -> auto& { return c.benchmark.trafficWarmupSeconds; },
            "Saturated traffic time excluded from measurements"),
        MakeStringOption(
            "wifi.band",
            [](auto& c) -> auto& { return c.wifi.band; },
            "Fixed Wi-Fi band"),
        MakeIntegerOption<uint16_t>(
            "wifi.channel_number",
            [](auto& c) -> auto& { return c.wifi.channelNumber; },
            "IEEE channel number"),
        MakeIntegerOption<uint16_t>(
            "wifi.bandwidth_mhz",
            [](auto& c) -> auto& { return c.wifi.bandwidthMhz; },
            "Channel width in MHz"),
        MakeIntegerOption<uint8_t>(
            "wifi.primary_20_index",
            [](auto& c) -> auto& { return c.wifi.primary20Index; },
            "Primary 20 MHz subchannel index"),
        MakeFloatOption(
            "wifi.tx_power_dbm",
            [](auto& c) -> auto& { return c.wifi.txPowerDbm; },
            "Fixed transmit power in dBm"),
        MakeStringOption(
            "wifi.rate_manager",
            [](auto& c) -> auto& { return c.wifi.rateManager; },
            "Fixed registered MinstrelHt Wi-Fi rate-manager TypeId"),
        MakeIntegerOption<uint32_t>(
            "wifi.guard_interval_ns",
            [](auto& c) -> auto& { return c.wifi.guardIntervalNs; },
            "Fixed HE guard interval in nanoseconds"),
        MakeIntegerOption<uint32_t>(
            "wifi.rts_cts_threshold_bytes",
            [](auto& c) -> auto& { return c.wifi.rtsCtsThresholdBytes; },
            "Fixed RTS/CTS threshold in bytes"),
        MakeIntegerOption<uint8_t>(
            "wifi.antennas",
            [](auto& c) -> auto& { return c.wifi.antennas; },
            "Antenna count"),
        MakeIntegerOption<uint8_t>(
            "wifi.max_tx_spatial_streams",
            [](auto& c) -> auto& { return c.wifi.maxTxSpatialStreams; },
            "Maximum transmit spatial streams"),
        MakeIntegerOption<uint8_t>(
            "wifi.max_rx_spatial_streams",
            [](auto& c) -> auto& { return c.wifi.maxRxSpatialStreams; },
            "Maximum receive spatial streams"),
        MakeStringOption(
            "tcp.congestion_control",
            [](auto& c) -> auto& { return c.tcp.congestionControl; },
            "Fixed registered TcpHighSpeed congestion-control TypeId"),
        MakeIntegerOption<uint32_t>(
            "tcp.segment_size_bytes",
            [](auto& c) -> auto& { return c.tcp.segmentSizeBytes; },
            "TCP maximum segment payload"),
        MakeIntegerOption<uint32_t>(
            "tcp.send_buffer_bytes",
            [](auto& c) -> auto& { return c.tcp.sendBufferBytes; },
            "TCP send-buffer size"),
        MakeIntegerOption<uint32_t>(
            "tcp.receive_buffer_bytes",
            [](auto& c) -> auto& { return c.tcp.receiveBufferBytes; },
            "TCP receive-buffer size"),
        MakeStringOption(
            "tcp.wired_rate",
            [](auto& c) -> auto& { return c.tcp.wiredRate; },
            "Dedicated wired-link data rate"),
        MakeStringOption(
            "tcp.wired_delay",
            [](auto& c) -> auto& { return c.tcp.wiredDelay; },
            "Dedicated wired-link delay"),
        MakeIntegerOption<uint32_t>(
            "statistics.window_ms",
            [](auto& c) -> auto& { return c.statistics.windowMs; },
            "Statistics window width in milliseconds"),
        MakeStringOption(
            "logging.scenario_level",
            [](auto& c) -> auto& { return c.logging.scenarioLevel; },
            "Saturated scenario log level")};
    return options;
}

} // namespace ns3
