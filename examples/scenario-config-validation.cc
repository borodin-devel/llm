#include "scenario-config.h"

#include "ns3/tcp-congestion-ops.h"
#include "ns3/wifi-remote-station-manager.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>

namespace ns3
{

namespace
{

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
    throw ScenarioConfigError("invalid " + std::string(path) + " (" + std::string(flag) +
                              "): expected " + std::string(expected) + "; got " +
                              std::string(actual));
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

void
RequireFiniteNonNegative(double value, std::string_view path, std::string_view flag)
{
    Require(std::isfinite(value) && value >= 0.0,
            path,
            flag,
            "finite value greater than or equal to 0",
            value);
}

void
ValidateLogLevel(std::string_view value, std::string_view path, std::string_view flag)
{
    try
    {
        static_cast<void>(ParseScenarioLogLevel(value));
    }
    catch (const ScenarioConfigError&)
    {
        ThrowInvalid(path, flag, "off, error, warn, info, debug, function, logic, or all", value);
    }
}

} // namespace

WifiPhyBand
ToWifiPhyBand(WifiBandConfig band)
{
    switch (band)
    {
    case WifiBandConfig::BAND_2_4_GHZ:
        return WIFI_PHY_BAND_2_4GHZ;
    case WifiBandConfig::BAND_5_GHZ:
        return WIFI_PHY_BAND_5GHZ;
    case WifiBandConfig::BAND_6_GHZ:
        return WIFI_PHY_BAND_6GHZ;
    }
    ThrowInvalid("wifi.band", "--wifi-band", "2.4GHz, 5GHz, or 6GHz", ToString(band));
}

std::optional<LogLevel>
ParseScenarioLogLevel(std::string_view level)
{
    static const std::array<std::pair<std::string_view, LogLevel>, 7> levels{{
        {"error", LOG_LEVEL_ERROR},
        {"warn", LOG_LEVEL_WARN},
        {"info", LOG_LEVEL_INFO},
        {"debug", LOG_LEVEL_DEBUG},
        {"function", LOG_LEVEL_FUNCTION},
        {"logic", LOG_LEVEL_LOGIC},
        {"all", LOG_LEVEL_ALL},
    }};
    if (level == "off")
    {
        return std::nullopt;
    }
    for (const auto& [name, value] : levels)
    {
        if (level == name)
        {
            return value;
        }
    }
    throw ScenarioConfigError("unknown scenario log level: " + std::string(level));
}

void
ConfigureScenarioLogging(const LoggingConfig& logging)
{
    const std::array components{
        std::pair{"SampleScenario", std::string_view(logging.sampleScenarioLevel)},
        std::pair{"APGenerator", std::string_view(logging.apGeneratorLevel)},
        std::pair{"StaLlmGenerator", std::string_view(logging.staGeneratorLevel)},
        std::pair{"TrafficSink", std::string_view(logging.trafficSinkLevel)},
        std::pair{"ContentionAwareAgentDistribution",
                  std::string_view(logging.contentionDistributionLevel)},
    };
    for (const auto& [name, configuredLevel] : components)
    {
        if (const auto level = ParseScenarioLogLevel(configuredLevel))
        {
            LogComponentEnable(name, *level);
        }
    }
}

TypeId
ResolveWifiManagerType(std::string_view name)
{
    TypeId type;
    const std::string typeName(name);
    if (!TypeId::LookupByNameFailSafe(typeName, &type) ||
        !type.IsChildOf(WifiRemoteStationManager::GetTypeId()))
    {
        ThrowInvalid("wifi.rate_manager",
                     "--wifi-rate-manager",
                     "registered TypeId derived from ns3::WifiRemoteStationManager",
                     name);
    }
    return type;
}

TypeId
ResolveTcpCongestionType(std::string_view name)
{
    TypeId type;
    const std::string typeName(name);
    if (!TypeId::LookupByNameFailSafe(typeName, &type) ||
        !type.IsChildOf(TcpCongestionOps::GetTypeId()))
    {
        ThrowInvalid("tcp.congestion_control",
                     "--tcp-congestion-control",
                     "registered TypeId derived from ns3::TcpCongestionOps",
                     name);
    }
    return type;
}

void
ValidateScenarioConfig(const ScenarioConfig& config)
{
    Require(!config.general.traceFile.empty(),
            "general.trace_file",
            "--general-trace-file",
            "non-empty string",
            "empty string");
    if (config.general.runFolder)
    {
        Require(!config.general.runFolder->empty(),
                "general.run_folder",
                "--general-run-folder",
                "non-empty path",
                "empty string");
    }

    const auto& outputName = config.general.outputName;
    const std::filesystem::path outputPath(outputName);
    const bool plainJsonName =
        !outputName.empty() && outputName.ends_with(".json") &&
        outputName.find('/') == std::string::npos && outputName.find('\\') == std::string::npos &&
        outputName.find("..") == std::string::npos && !outputPath.has_root_name() &&
        !outputPath.has_parent_path() && outputPath.filename() == outputPath;
    Require(plainJsonName,
            "general.output_name",
            "--general-output-name",
            "plain filename ending in .json",
            outputName.empty() ? "empty string" : outputName);

    Require(config.simulation.durationMode == DurationMode::AUTO ||
                config.simulation.durationMode == DurationMode::FIXED,
            "simulation.duration_mode",
            "--simulation-duration-mode",
            "auto or fixed",
            config.simulation.durationMode);
    RequireFiniteNonNegative(config.simulation.fixedDurationSeconds,
                             "simulation.fixed_duration_seconds",
                             "--simulation-fixed-duration-seconds");
    if (config.simulation.durationMode == DurationMode::FIXED)
    {
        Require(config.simulation.fixedDurationSeconds > 0.0,
                "simulation.fixed_duration_seconds",
                "--simulation-fixed-duration-seconds",
                "finite value greater than 0 in fixed mode",
                config.simulation.fixedDurationSeconds);
    }
    RequireFiniteNonNegative(config.simulation.autoTailSeconds,
                             "simulation.auto_tail_seconds",
                             "--simulation-auto-tail-seconds");

    Require(config.topology.bssCount > 0,
            "topology.bss_count",
            "--topology-bss-count",
            "value greater than 0",
            config.topology.bssCount);
    Require(config.topology.stationsPerBss > 0,
            "topology.stations_per_bss",
            "--topology-stations-per-bss",
            "value greater than 0",
            config.topology.stationsPerBss);
    RequireFiniteNonNegative(config.topology.bssSpacingM,
                             "topology.bss_spacing_m",
                             "--topology-bss-spacing-m");
    RequireFiniteNonNegative(config.topology.stationRadiusM,
                             "topology.station_radius_m",
                             "--topology-station-radius-m");
    Require(!config.topology.ssidPrefix.empty(),
            "topology.ssid_prefix",
            "--topology-ssid-prefix",
            "non-empty string",
            "empty string");
    Require(config.topology.apSinkPort != 0,
            "topology.ap_sink_port",
            "--topology-ap-sink-port",
            "nonzero TCP port",
            config.topology.apSinkPort);
    Require(config.topology.stationSinkBasePort != 0,
            "topology.station_sink_base_port",
            "--topology-station-sink-base-port",
            "nonzero TCP port",
            config.topology.stationSinkBasePort);
    const auto extraStationPorts = static_cast<uint64_t>(config.topology.stationsPerBss - 1);
    const auto availableStationPorts = static_cast<uint64_t>(std::numeric_limits<uint16_t>::max() -
                                                             config.topology.stationSinkBasePort);
    Require(extraStationPorts <= availableStationPorts,
            "topology.station_sink_base_port",
            "--topology-station-sink-base-port",
            "base port plus stations per BSS minus 1 no greater than 65535",
            config.topology.stationSinkBasePort);
    RequireFiniteNonNegative(config.topology.generatorStartSeconds,
                             "topology.generator_start_seconds",
                             "--topology-generator-start-seconds");

    Require(config.distribution.maxAgentsPerStation >= 0,
            "distribution.max_agents_per_station",
            "--distribution-max-agents-per-station",
            "value greater than or equal to 0",
            config.distribution.maxAgentsPerStation);
    Require(config.distribution.slotMs > 0,
            "distribution.slot_ms",
            "--distribution-slot-ms",
            "value greater than 0",
            config.distribution.slotMs);

    static_cast<void>(ToWifiPhyBand(config.wifi.band));
    const bool validWidth = config.wifi.bandwidthMhz == 20 || config.wifi.bandwidthMhz == 40 ||
                            config.wifi.bandwidthMhz == 80 || config.wifi.bandwidthMhz == 160;
    Require(validWidth,
            "wifi.bandwidth_mhz",
            "--wifi-bandwidth-mhz",
            "20, 40, 80, or 160",
            config.wifi.bandwidthMhz);
    Require(config.wifi.primary20Index < config.wifi.bandwidthMhz / 20,
            "wifi.primary_20_index",
            "--wifi-primary-20-index",
            "index less than bandwidth_mhz / 20",
            static_cast<unsigned int>(config.wifi.primary20Index));
    static_cast<void>(ResolveWifiManagerType(config.wifi.rateManager));

    static_cast<void>(ResolveTcpCongestionType(config.tcp.congestionControl));
    Require(config.tcp.segmentSizeBytes > 0,
            "tcp.segment_size_bytes",
            "--tcp-segment-size-bytes",
            "value greater than 0",
            config.tcp.segmentSizeBytes);
    Require(config.tcp.sendBufferBytes > 0,
            "tcp.send_buffer_bytes",
            "--tcp-send-buffer-bytes",
            "value greater than 0",
            config.tcp.sendBufferBytes);
    Require(config.tcp.receiveBufferBytes > 0,
            "tcp.receive_buffer_bytes",
            "--tcp-receive-buffer-bytes",
            "value greater than 0",
            config.tcp.receiveBufferBytes);
    Require(config.tcp.segmentSizeBytes <= config.tcp.sendBufferBytes &&
                config.tcp.segmentSizeBytes <= config.tcp.receiveBufferBytes,
            "tcp.segment_size_bytes",
            "--tcp-segment-size-bytes",
            "value no greater than send_buffer_bytes or receive_buffer_bytes",
            config.tcp.segmentSizeBytes);

    Require(config.statistics.windowMs > 0,
            "statistics.window_ms",
            "--statistics-window-ms",
            "value greater than 0",
            config.statistics.windowMs);
    const std::array logLevels{
        std::tuple{std::string_view(config.logging.sampleScenarioLevel),
                   std::string_view("logging.sample_scenario_level"),
                   std::string_view("--logging-sample-scenario-level")},
        std::tuple{std::string_view(config.logging.apGeneratorLevel),
                   std::string_view("logging.ap_generator_level"),
                   std::string_view("--logging-ap-generator-level")},
        std::tuple{std::string_view(config.logging.staGeneratorLevel),
                   std::string_view("logging.sta_generator_level"),
                   std::string_view("--logging-sta-generator-level")},
        std::tuple{std::string_view(config.logging.trafficSinkLevel),
                   std::string_view("logging.traffic_sink_level"),
                   std::string_view("--logging-traffic-sink-level")},
        std::tuple{std::string_view(config.logging.contentionDistributionLevel),
                   std::string_view("logging.contention_distribution_level"),
                   std::string_view("--logging-contention-distribution-level")},
    };
    for (const auto& [value, path, flag] : logLevels)
    {
        ValidateLogLevel(value, path, flag);
    }
}

} // namespace ns3
