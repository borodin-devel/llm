#include "config.h"

#include "ns3/nstime.h"
#include "ns3/tcp-congestion-ops.h"
#include "ns3/type-id.h"
#include "ns3/wifi-remote-station-manager.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace ns3
{

namespace
{

constexpr uint32_t kMaximumRngSeed = 4294944442U;

template <typename T>
std::string
ToString(const T& value)
{
    std::ostringstream output;
    output << value;
    return output.str();
}

[[noreturn]] void
ThrowInvalid(std::string_view path,
             std::string_view flag,
             std::string_view expected,
             std::string_view actual)
{
    throw SaturatedTcpConfigError("invalid saturated " + std::string(path) + " (" +
                                  std::string(flag) + "): expected " + std::string(expected) +
                                  "; got " + std::string(actual));
}

template <typename T>
void
Require(bool condition,
        std::string_view path,
        std::string_view flag,
        std::string_view expected,
        const T& actual)
{
    if (!condition)
    {
        ThrowInvalid(path, flag, expected, ToString(actual));
    }
}

bool
IsKnownLogLevel(std::string_view level)
{
    static constexpr std::array levels{
        std::string_view("off"),
        std::string_view("error"),
        std::string_view("warn"),
        std::string_view("info"),
        std::string_view("debug"),
        std::string_view("function"),
        std::string_view("logic"),
        std::string_view("all"),
    };
    for (const auto candidate : levels)
    {
        if (level == candidate)
        {
            return true;
        }
    }
    return false;
}

/**
 * Parse an ns-3 data-rate spelling without invoking the unsafe stream extractor.
 *
 * This mirrors the private DataRate::DoParse grammar while requiring numeric
 * extraction to succeed before assigning the initialized result.
 *
 * @param text Data-rate spelling to parse.
 * @param rate Parsed bit rate destination.
 * @return Whether parsing succeeded.
 */
bool
ParseDataRate(std::string_view text, uint64_t* rate)
{
    const auto suffixStart = text.find_first_not_of("0123456789. ");
    if (suffixStart == std::string_view::npos)
    {
        std::istringstream input{std::string(text)};
        input >> *rate;
        return !input.fail();
    }

    static constexpr std::array<std::pair<std::string_view, long double>, 26> units{{
        {"bps", 1.0L},
        {"b/s", 1.0L},
        {"Bps", 8.0L},
        {"B/s", 8.0L},
        {"kbps", 1000.0L},
        {"kb/s", 1000.0L},
        {"Kbps", 1000.0L},
        {"Kb/s", 1000.0L},
        {"kBps", 8000.0L},
        {"kB/s", 8000.0L},
        {"KBps", 8000.0L},
        {"KB/s", 8000.0L},
        {"Kib/s", 1024.0L},
        {"KiB/s", 8192.0L},
        {"Mbps", 1000000.0L},
        {"Mb/s", 1000000.0L},
        {"MBps", 8000000.0L},
        {"MB/s", 8000000.0L},
        {"Mib/s", 1048576.0L},
        {"MiB/s", 8388608.0L},
        {"Gbps", 1000000000.0L},
        {"Gb/s", 1000000000.0L},
        {"GBps", 8000000000.0L},
        {"GB/s", 8000000000.0L},
        {"Gib/s", 1073741824.0L},
        {"GiB/s", 8589934592.0L},
    }};
    const auto suffix = text.substr(suffixStart);
    for (const auto& [name, multiplier] : units)
    {
        if (suffix != name)
        {
            continue;
        }

        std::istringstream input{std::string(text.substr(0, suffixStart))};
        double quantity{};
        input >> quantity;
        const long double bitsPerSecond = static_cast<long double>(quantity) * multiplier;
        if (input.fail() || !std::isfinite(quantity) || bitsPerSecond < 0.0L ||
            bitsPerSecond > static_cast<long double>(std::numeric_limits<uint64_t>::max()))
        {
            return false;
        }
        *rate = static_cast<uint64_t>(bitsPerSecond);
        return true;
    }
    return false;
}

bool
IsPositiveDataRate(std::string_view text)
{
    uint64_t rate = 0;
    return ParseDataRate(text, &rate) && rate > 0;
}

bool
IsPositiveDuration(std::string_view text)
{
    if (text.empty() || text.find_first_of(" \t\r\n\f\v") != std::string_view::npos)
    {
        return false;
    }

    const auto suffixStart = text.find_first_not_of("+-0123456789.eE");
    const std::string_view numeric = text.substr(0, suffixStart);
    const std::string_view suffix =
        suffixStart == std::string_view::npos ? std::string_view{} : text.substr(suffixStart);
    static constexpr std::array<std::pair<std::string_view, Time::Unit>, 10> units{{
        {"s", Time::S},
        {"ms", Time::MS},
        {"us", Time::US},
        {"ns", Time::NS},
        {"ps", Time::PS},
        {"fs", Time::FS},
        {"min", Time::MIN},
        {"h", Time::H},
        {"d", Time::D},
        {"y", Time::Y},
    }};
    Time::Unit unit = Time::S;
    if (!suffix.empty())
    {
        bool knownUnit = false;
        for (const auto& [name, candidate] : units)
        {
            if (suffix == name)
            {
                unit = candidate;
                knownUnit = true;
                break;
            }
        }
        if (!knownUnit)
        {
            return false;
        }
    }

    std::istringstream numericInput{std::string(numeric)};
    double quantity{};
    numericInput >> quantity;
    if (numericInput.fail() || !std::isfinite(quantity) || quantity <= 0.0 ||
        quantity > Time::Max().ToDouble(unit))
    {
        return false;
    }

    TimeValue value;
    return value.DeserializeFromString(std::string(text), MakeTimeChecker()) &&
           value.Get().IsStrictlyPositive();
}

void
ValidateEnumValues(const SaturatedTcpConfig& config)
{
    Require(config.benchmark.rssiRange == SaturatedRssiRange::HIGH ||
                config.benchmark.rssiRange == SaturatedRssiRange::MEDIUM ||
                config.benchmark.rssiRange == SaturatedRssiRange::LOW,
            "benchmark.rssi_range",
            "--benchmark-rssi-range",
            "high, medium, or low",
            config.benchmark.rssiRange);
    Require(config.benchmark.interferenceMode == SaturatedInterferenceMode::ISOLATED ||
                config.benchmark.interferenceMode == SaturatedInterferenceMode::AP_ONLY_COCHANNEL,
            "benchmark.interference_mode",
            "--benchmark-interference-mode",
            "isolated or ap_only_cochannel",
            config.benchmark.interferenceMode);
    Require(config.benchmark.trafficMode == SaturatedTrafficMode::UL ||
                config.benchmark.trafficMode == SaturatedTrafficMode::DL ||
                config.benchmark.trafficMode == SaturatedTrafficMode::UL_DL,
            "benchmark.traffic_mode",
            "--benchmark-traffic-mode",
            "ul, dl, or ul_dl",
            config.benchmark.trafficMode);
    if (config.benchmark.mimoMode == SaturatedMimoMode::MU)
    {
        throw SaturatedTcpConfigError(
            "invalid saturated benchmark.mimo_mode (--benchmark-mimo-mode): "
            "DL MU-MIMO is not supported; use su");
    }
    Require(config.benchmark.mimoMode == SaturatedMimoMode::SU,
            "benchmark.mimo_mode",
            "--benchmark-mimo-mode",
            "su",
            config.benchmark.mimoMode);
}

void
ValidateTypeIds(const SaturatedTcpConfig& config)
{
    TypeId type;
    if (!TypeId::LookupByNameFailSafe(config.wifi.rateManager, &type) ||
        !type.IsChildOf(WifiRemoteStationManager::GetTypeId()))
    {
        ThrowInvalid("wifi.rate_manager",
                     "--wifi-rate-manager",
                     "registered TypeId derived from ns3::WifiRemoteStationManager",
                     config.wifi.rateManager);
    }
    Require(config.wifi.rateManager == "ns3::MinstrelHtWifiManager",
            "wifi.rate_manager",
            "--wifi-rate-manager",
            "ns3::MinstrelHtWifiManager",
            config.wifi.rateManager);
    if (!TypeId::LookupByNameFailSafe(config.tcp.congestionControl, &type) ||
        !type.IsChildOf(TcpCongestionOps::GetTypeId()))
    {
        ThrowInvalid("tcp.congestion_control",
                     "--tcp-congestion-control",
                     "registered TypeId derived from ns3::TcpCongestionOps",
                     config.tcp.congestionControl);
    }
    Require(config.tcp.congestionControl == "ns3::TcpHighSpeed",
            "tcp.congestion_control",
            "--tcp-congestion-control",
            "ns3::TcpHighSpeed",
            config.tcp.congestionControl);
}

} // namespace

void
ValidateSaturatedTcpConfig(const SaturatedTcpConfig& config)
{
    if (config.general.runFolder)
    {
        Require(!config.general.runFolder->empty() &&
                    config.general.runFolder->find('\0') == std::string::npos,
                "general.run_folder",
                "--general-run-folder",
                "non-empty path without NUL bytes when supplied",
                config.general.runFolder->empty() ? "empty string" : *config.general.runFolder);
    }

    const auto& outputName = config.general.outputName;
    const std::filesystem::path outputPath(outputName);
    const bool plainJsonName =
        !outputName.empty() && outputName.ends_with(".json") &&
        outputName.find('\0') == std::string::npos && outputName.find('/') == std::string::npos &&
        outputName.find('\\') == std::string::npos && outputName.find("..") == std::string::npos &&
        !outputPath.has_root_name() && !outputPath.has_parent_path() &&
        outputPath.filename() == outputPath;
    Require(plainJsonName,
            "general.output_name",
            "--general-output-name",
            "plain filename ending in .json",
            outputName.empty() ? "empty string" : outputName);

    Require(config.script.repetitions > 0,
            "script.repetitions",
            "--script-repetitions",
            "positive metadata integer; C++ still executes one run",
            config.script.repetitions);
    Require(config.simulation.rngSeed >= 1 && config.simulation.rngSeed <= kMaximumRngSeed,
            "simulation.rng_seed",
            "--simulation-rng-seed",
            "integer from 1 through 4294944442",
            config.simulation.rngSeed);
    Require(config.simulation.rngRun > 0,
            "simulation.rng_run",
            "--simulation-rng-run",
            "positive integer",
            config.simulation.rngRun);
    Require(config.benchmark.stationCountPerBss >= 1 && config.benchmark.stationCountPerBss <= 30,
            "benchmark.sta_count_per_bss",
            "--benchmark-sta-count-per-bss",
            "integer from 1 through 30",
            config.benchmark.stationCountPerBss);
    ValidateEnumValues(config);

    Require(config.wifi.band == "5GHz", "wifi.band", "--wifi-band", "5GHz", config.wifi.band);
    Require(config.wifi.channelNumber == 42,
            "wifi.channel_number",
            "--wifi-channel-number",
            "42",
            config.wifi.channelNumber);
    Require(config.wifi.bandwidthMhz == 80,
            "wifi.bandwidth_mhz",
            "--wifi-bandwidth-mhz",
            "80",
            config.wifi.bandwidthMhz);
    Require(config.wifi.primary20Index == 0,
            "wifi.primary_20_index",
            "--wifi-primary-20-index",
            "0",
            static_cast<unsigned int>(config.wifi.primary20Index));
    Require(config.wifi.txPowerDbm == 20.0,
            "wifi.tx_power_dbm",
            "--wifi-tx-power-dbm",
            "20.0",
            config.wifi.txPowerDbm);
    Require(config.wifi.antennas == 2,
            "wifi.antennas",
            "--wifi-antennas",
            "2",
            static_cast<unsigned int>(config.wifi.antennas));
    Require(config.wifi.maxTxSpatialStreams == 2,
            "wifi.max_tx_spatial_streams",
            "--wifi-max-tx-spatial-streams",
            "2",
            static_cast<unsigned int>(config.wifi.maxTxSpatialStreams));
    Require(config.wifi.maxRxSpatialStreams == 2,
            "wifi.max_rx_spatial_streams",
            "--wifi-max-rx-spatial-streams",
            "2",
            static_cast<unsigned int>(config.wifi.maxRxSpatialStreams));
    ValidateTypeIds(config);

    Require(config.tcp.segmentSizeBytes > 0,
            "tcp.segment_size_bytes",
            "--tcp-segment-size-bytes",
            "positive integer",
            config.tcp.segmentSizeBytes);
    Require(config.tcp.sendBufferBytes > 0,
            "tcp.send_buffer_bytes",
            "--tcp-send-buffer-bytes",
            "positive integer",
            config.tcp.sendBufferBytes);
    Require(config.tcp.receiveBufferBytes > 0,
            "tcp.receive_buffer_bytes",
            "--tcp-receive-buffer-bytes",
            "positive integer",
            config.tcp.receiveBufferBytes);
    Require(config.tcp.segmentSizeBytes <= config.tcp.sendBufferBytes &&
                config.tcp.segmentSizeBytes <= config.tcp.receiveBufferBytes,
            "tcp.segment_size_bytes",
            "--tcp-segment-size-bytes",
            "value no greater than send_buffer_bytes or receive_buffer_bytes",
            config.tcp.segmentSizeBytes);
    Require(IsPositiveDataRate(config.tcp.wiredRate),
            "tcp.wired_rate",
            "--tcp-wired-rate",
            "positive finite ns-3 data-rate string",
            config.tcp.wiredRate);
    Require(IsPositiveDuration(config.tcp.wiredDelay),
            "tcp.wired_delay",
            "--tcp-wired-delay",
            "positive finite ns-3 duration string",
            config.tcp.wiredDelay);

    Require(config.statistics.windowMs > 0 && 1000 % config.statistics.windowMs == 0,
            "statistics.window_ms",
            "--statistics-window-ms",
            "positive integer that must divide 1000 exactly",
            config.statistics.windowMs);
    Require(IsKnownLogLevel(config.logging.scenarioLevel),
            "logging.scenario_level",
            "--logging-scenario-level",
            "off, error, warn, info, debug, function, logic, or all",
            config.logging.scenarioLevel);
}

} // namespace ns3
