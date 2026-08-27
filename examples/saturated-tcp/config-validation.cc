#include "config-internal.h"

#include "ns3/nstime.h"
#include "ns3/tcp-congestion-ops.h"
#include "ns3/type-id.h"
#include "ns3/wifi-remote-station-manager.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ns3
{

namespace
{

constexpr uint32_t kMaximumRngSeed = 4294944442U;
constexpr uint64_t kRequiredWiredRateBps = 10'000'000'000ULL;
constexpr int64_t kRequiredWiredDelayNs = 100'000;

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
 * This preserves the private DataRate::DoParse unit spellings and boundaries
 * while scaling the decimal token exactly, requiring complete consumption and
 * an integral result before assigning the initialized destination.
 *
 * @param text Data-rate spelling to parse.
 * @param rate Parsed bit rate destination.
 * @return Whether parsing succeeded.
 */
bool
ParseDataRate(std::string_view text, uint64_t* rate)
{
    *rate = 0;
    const auto suffixStart = text.find_first_not_of("0123456789. ");
    auto numeric = text.substr(0, suffixStart);
    const std::string_view suffix =
        suffixStart == std::string_view::npos ? std::string_view{} : text.substr(suffixStart);

    static constexpr std::array<std::pair<std::string_view, uint64_t>, 26> units{{
        {"bps", 1},
        {"b/s", 1},
        {"Bps", 8},
        {"B/s", 8},
        {"kbps", 1000},
        {"kb/s", 1000},
        {"Kbps", 1000},
        {"Kb/s", 1000},
        {"kBps", 8000},
        {"kB/s", 8000},
        {"KBps", 8000},
        {"KB/s", 8000},
        {"Kib/s", 1024},
        {"KiB/s", 8192},
        {"Mbps", 1000000},
        {"Mb/s", 1000000},
        {"MBps", 8000000},
        {"MB/s", 8000000},
        {"Mib/s", 1048576},
        {"MiB/s", 8388608},
        {"Gbps", 1000000000},
        {"Gb/s", 1000000000},
        {"GBps", 8000000000},
        {"GB/s", 8000000000},
        {"Gib/s", 1073741824},
        {"GiB/s", 8589934592},
    }};

    uint64_t multiplier = 1;
    bool knownUnit = suffix.empty();
    for (const auto& [name, candidate] : units)
    {
        if (suffix == name)
        {
            multiplier = candidate;
            knownUnit = true;
            break;
        }
    }
    if (!knownUnit)
    {
        return false;
    }

    const auto firstDigit = numeric.find_first_not_of(' ');
    if (firstDigit == std::string_view::npos)
    {
        return false;
    }
    const auto lastDigit = numeric.find_last_not_of(' ');
    numeric = numeric.substr(firstDigit, lastDigit - firstDigit + 1);

    std::vector<uint8_t> digits;
    digits.reserve(numeric.size() + 10);
    std::size_t fractionalDigits = 0;
    bool decimalPointSeen = false;
    for (const char character : numeric)
    {
        if (character == '.')
        {
            if (decimalPointSeen)
            {
                return false;
            }
            decimalPointSeen = true;
            continue;
        }
        if (character < '0' || character > '9')
        {
            return false;
        }
        digits.push_back(static_cast<uint8_t>(character - '0'));
        fractionalDigits += decimalPointSeen;
    }
    if (digits.empty())
    {
        return false;
    }

    // Multiply little-endian decimal digits to avoid binary floating-point rounding.
    std::reverse(digits.begin(), digits.end());
    uint64_t carry = 0;
    for (auto& digit : digits)
    {
        const uint64_t product = static_cast<uint64_t>(digit) * multiplier + carry;
        digit = static_cast<uint8_t>(product % 10);
        carry = product / 10;
    }
    while (carry > 0)
    {
        digits.push_back(static_cast<uint8_t>(carry % 10));
        carry /= 10;
    }

    // Decimal scaling is integral only when every discarded product digit is zero.
    if (fractionalDigits > digits.size())
    {
        return false;
    }
    for (std::size_t i = 0; i < fractionalDigits; ++i)
    {
        if (digits[i] != 0)
        {
            return false;
        }
    }

    uint64_t bitsPerSecond = 0;
    for (std::size_t i = digits.size(); i > fractionalDigits; --i)
    {
        const uint64_t digit = digits[i - 1];
        if (bitsPerSecond > (std::numeric_limits<uint64_t>::max() - digit) / 10)
        {
            return false;
        }
        bitsPerSecond = bitsPerSecond * 10 + digit;
    }
    if (bitsPerSecond == 0)
    {
        return false;
    }
    *rate = bitsPerSecond;
    return true;
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

std::optional<uint64_t>
ParseSaturatedTcpDataRate(std::string_view text)
{
    uint64_t rate = 0;
    if (!ParseDataRate(text, &rate))
    {
        return std::nullopt;
    }
    return rate;
}

std::optional<int64_t>
ParseSaturatedTcpDurationNs(std::string_view text)
{
    if (!IsPositiveDuration(text))
    {
        return std::nullopt;
    }
    TimeValue value;
    if (!value.DeserializeFromString(std::string(text), MakeTimeChecker()))
    {
        return std::nullopt;
    }
    return value.Get().GetNanoSeconds();
}

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
    Require(config.wifi.guardIntervalNs == 3200,
            "wifi.guard_interval_ns",
            "--wifi-guard-interval-ns",
            "3200",
            config.wifi.guardIntervalNs);
    Require(config.wifi.rtsCtsThresholdBytes == 0,
            "wifi.rts_cts_threshold_bytes",
            "--wifi-rts-cts-threshold-bytes",
            "0",
            config.wifi.rtsCtsThresholdBytes);
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
    const auto wiredRate = ParseSaturatedTcpDataRate(config.tcp.wiredRate);
    Require(wiredRate && *wiredRate == kRequiredWiredRateBps,
            "tcp.wired_rate",
            "--tcp-wired-rate",
            "data rate equal to exactly 10000000000 bps (10Gbps)",
            config.tcp.wiredRate);
    const auto wiredDelayNs = ParseSaturatedTcpDurationNs(config.tcp.wiredDelay);
    Require(wiredDelayNs && *wiredDelayNs == kRequiredWiredDelayNs,
            "tcp.wired_delay",
            "--tcp-wired-delay",
            "duration equal to exactly 100000 ns (0.1ms)",
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
