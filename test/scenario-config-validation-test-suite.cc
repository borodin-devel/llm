#include "../examples/scenario-config.h"
#include "llm-test-suite.h"

#include "ns3/log.h"
#include "ns3/type-id.h"
#include "ns3/wifi-phy-band.h"

#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace ns3;

namespace
{

ScenarioConfig
MakeValidConfig()
{
    ScenarioConfig config;
    config.general.traceFile = "trace.json";
    return config;
}

struct InvalidConfigCase
{
    std::string_view name;
    std::string_view tomlPath;
    std::string_view cliFlag;
    std::function<void(ScenarioConfig&)> mutate;
};

/**
 * @ingroup tests
 *
 * Verify single-field and cross-field scenario validation.
 */
class ScenarioConfigValidationTestCase : public TestCase
{
  public:
    ScenarioConfigValidationTestCase();

  private:
    void DoRun() override;
    void CheckInvalid(const InvalidConfigCase& testCase);
};

ScenarioConfigValidationTestCase::ScenarioConfigValidationTestCase()
    : TestCase("scenario configuration validation boundaries")
{
}

void
ScenarioConfigValidationTestCase::CheckInvalid(const InvalidConfigCase& testCase)
{
    auto config = MakeValidConfig();
    ValidateScenarioConfig(config);
    testCase.mutate(config);

    try
    {
        ValidateScenarioConfig(config);
        NS_TEST_ASSERT_MSG_EQ(true, false, "Invalid case accepted: " << testCase.name);
    }
    catch (const ScenarioConfigError& error)
    {
        const std::string message = error.what();
        NS_TEST_ASSERT_MSG_NE(message.find(testCase.tomlPath),
                              std::string::npos,
                              "Error lacks TOML key for " << testCase.name << ": " << message);
        NS_TEST_ASSERT_MSG_NE(message.find(testCase.cliFlag),
                              std::string::npos,
                              "Error lacks CLI flag for " << testCase.name << ": " << message);
    }
}

void
ScenarioConfigValidationTestCase::DoRun()
{
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double infinity = std::numeric_limits<double>::infinity();
    const std::vector<InvalidConfigCase> invalidCases{
        {"empty trace",
         "general.trace_file",
         "--general-trace-file",
         [](auto& c) { c.general.traceFile.clear(); }},
        {"empty run folder",
         "general.run_folder",
         "--general-run-folder",
         [](auto& c) { c.general.runFolder = ""; }},
        {"empty output name",
         "general.output_name",
         "--general-output-name",
         [](auto& c) { c.general.outputName.clear(); }},
        {"nested output name",
         "general.output_name",
         "--general-output-name",
         [](auto& c) { c.general.outputName = "nested/path.json"; }},
        {"backslash output name",
         "general.output_name",
         "--general-output-name",
         [](auto& c) { c.general.outputName = "nested\\path.json"; }},
        {"parent marker output name",
         "general.output_name",
         "--general-output-name",
         [](auto& c) { c.general.outputName = "..json"; }},
        {"non-JSON output name",
         "general.output_name",
         "--general-output-name",
         [](auto& c) { c.general.outputName = "result.txt"; }},
        {"unknown duration mode",
         "simulation.duration_mode",
         "--simulation-duration-mode",
         [](auto& c) { c.simulation.durationMode = static_cast<DurationMode>(99); }},
        {"zero fixed duration",
         "simulation.fixed_duration_seconds",
         "--simulation-fixed-duration-seconds",
         [](auto& c) { c.simulation.durationMode = DurationMode::FIXED; }},
        {"negative automatic duration",
         "simulation.fixed_duration_seconds",
         "--simulation-fixed-duration-seconds",
         [](auto& c) { c.simulation.fixedDurationSeconds = -1.0; }},
        {"NaN duration",
         "simulation.fixed_duration_seconds",
         "--simulation-fixed-duration-seconds",
         [nan](auto& c) { c.simulation.fixedDurationSeconds = nan; }},
        {"infinite duration",
         "simulation.fixed_duration_seconds",
         "--simulation-fixed-duration-seconds",
         [infinity](auto& c) { c.simulation.fixedDurationSeconds = infinity; }},
        {"negative automatic tail",
         "simulation.auto_tail_seconds",
         "--simulation-auto-tail-seconds",
         [](auto& c) { c.simulation.autoTailSeconds = -1.0; }},
        {"NaN automatic tail",
         "simulation.auto_tail_seconds",
         "--simulation-auto-tail-seconds",
         [nan](auto& c) { c.simulation.autoTailSeconds = nan; }},
        {"infinite automatic tail",
         "simulation.auto_tail_seconds",
         "--simulation-auto-tail-seconds",
         [infinity](auto& c) { c.simulation.autoTailSeconds = infinity; }},
        {"zero RNG seed",
         "simulation.rng_seed",
         "--simulation-rng-seed",
         [](auto& c) { c.simulation.rngSeed = 0; }},
        {"RNG seed above RngStream maximum",
         "simulation.rng_seed",
         "--simulation-rng-seed",
         [](auto& c) { c.simulation.rngSeed = 4294944443U; }},
        {"zero BSS count",
         "topology.bss_count",
         "--topology-bss-count",
         [](auto& c) { c.topology.bssCount = 0; }},
        {"negative BSS count",
         "topology.bss_count",
         "--topology-bss-count",
         [](auto& c) { c.topology.bssCount = -1; }},
        {"BSS count exceeds IPv4 subnet range",
         "topology.bss_count",
         "--topology-bss-count",
         [](auto& c) { c.topology.bssCount = 257; }},
        {"zero station count",
         "topology.stations_per_bss",
         "--topology-stations-per-bss",
         [](auto& c) { c.topology.stationsPerBss = 0; }},
        {"negative station count",
         "topology.stations_per_bss",
         "--topology-stations-per-bss",
         [](auto& c) { c.topology.stationsPerBss = -1; }},
        {"station count exceeds IPv4 host range",
         "topology.stations_per_bss",
         "--topology-stations-per-bss",
         [](auto& c) { c.topology.stationsPerBss = 254; }},
        {"negative BSS spacing",
         "topology.bss_spacing_m",
         "--topology-bss-spacing-m",
         [](auto& c) { c.topology.bssSpacingM = -1.0; }},
        {"NaN BSS spacing",
         "topology.bss_spacing_m",
         "--topology-bss-spacing-m",
         [nan](auto& c) { c.topology.bssSpacingM = nan; }},
        {"infinite BSS spacing",
         "topology.bss_spacing_m",
         "--topology-bss-spacing-m",
         [infinity](auto& c) { c.topology.bssSpacingM = infinity; }},
        {"negative station radius",
         "topology.station_radius_m",
         "--topology-station-radius-m",
         [](auto& c) { c.topology.stationRadiusM = -1.0; }},
        {"NaN station radius",
         "topology.station_radius_m",
         "--topology-station-radius-m",
         [nan](auto& c) { c.topology.stationRadiusM = nan; }},
        {"infinite station radius",
         "topology.station_radius_m",
         "--topology-station-radius-m",
         [infinity](auto& c) { c.topology.stationRadiusM = infinity; }},
        {"empty SSID prefix",
         "topology.ssid_prefix",
         "--topology-ssid-prefix",
         [](auto& c) { c.topology.ssidPrefix.clear(); }},
        {"SSID exceeds 32 bytes for one BSS",
         "topology.ssid_prefix",
         "--topology-ssid-prefix",
         [](auto& c) {
             c.topology.bssCount = 1;
             c.topology.ssidPrefix = std::string(32, 's');
         }},
        {"SSID exceeds 32 bytes at BSS 255",
         "topology.ssid_prefix",
         "--topology-ssid-prefix",
         [](auto& c) {
             c.topology.bssCount = 256;
             c.topology.ssidPrefix = std::string(30, 's');
         }},
        {"zero AP port",
         "topology.ap_sink_port",
         "--topology-ap-sink-port",
         [](auto& c) { c.topology.apSinkPort = 0; }},
        {"zero station port",
         "topology.station_sink_base_port",
         "--topology-station-sink-base-port",
         [](auto& c) { c.topology.stationSinkBasePort = 0; }},
        {"station port overflow",
         "topology.station_sink_base_port",
         "--topology-station-sink-base-port",
         [](auto& c) {
             c.topology.stationSinkBasePort = 65535;
             c.topology.stationsPerBss = 2;
         }},
        {"negative generator start",
         "topology.generator_start_seconds",
         "--topology-generator-start-seconds",
         [](auto& c) { c.topology.generatorStartSeconds = -1.0; }},
        {"NaN generator start",
         "topology.generator_start_seconds",
         "--topology-generator-start-seconds",
         [nan](auto& c) { c.topology.generatorStartSeconds = nan; }},
        {"infinite generator start",
         "topology.generator_start_seconds",
         "--topology-generator-start-seconds",
         [infinity](auto& c) { c.topology.generatorStartSeconds = infinity; }},
        {"negative station agent cap",
         "distribution.max_agents_per_station",
         "--distribution-max-agents-per-station",
         [](auto& c) { c.distribution.maxAgentsPerStation = -1; }},
        {"zero distribution slot",
         "distribution.slot_ms",
         "--distribution-slot-ms",
         [](auto& c) { c.distribution.slotMs = 0; }},
        {"negative distribution slot",
         "distribution.slot_ms",
         "--distribution-slot-ms",
         [](auto& c) { c.distribution.slotMs = -1; }},
        {"unknown Wi-Fi band",
         "wifi.band",
         "--wifi-band",
         [](auto& c) { c.wifi.band = static_cast<WifiBandConfig>(99); }},
        {"invalid Wi-Fi bandwidth",
         "wifi.bandwidth_mhz",
         "--wifi-bandwidth-mhz",
         [](auto& c) { c.wifi.bandwidthMhz = 30; }},
        {"invalid primary 20 index",
         "wifi.primary_20_index",
         "--wifi-primary-20-index",
         [](auto& c) { c.wifi.primary20Index = 1; }},
        {"unknown Wi-Fi manager",
         "wifi.rate_manager",
         "--wifi-rate-manager",
         [](auto& c) { c.wifi.rateManager = "ns3::MissingWifiManager"; }},
        {"wrong-parent Wi-Fi manager",
         "wifi.rate_manager",
         "--wifi-rate-manager",
         [](auto& c) { c.wifi.rateManager = "ns3::TcpHighSpeed"; }},
        {"unknown TCP congestion type",
         "tcp.congestion_control",
         "--tcp-congestion-control",
         [](auto& c) { c.tcp.congestionControl = "ns3::MissingTcpCongestionOps"; }},
        {"wrong-parent TCP congestion type",
         "tcp.congestion_control",
         "--tcp-congestion-control",
         [](auto& c) { c.tcp.congestionControl = "ns3::MinstrelHtWifiManager"; }},
        {"zero TCP segment",
         "tcp.segment_size_bytes",
         "--tcp-segment-size-bytes",
         [](auto& c) { c.tcp.segmentSizeBytes = 0; }},
        {"zero TCP send buffer",
         "tcp.send_buffer_bytes",
         "--tcp-send-buffer-bytes",
         [](auto& c) { c.tcp.sendBufferBytes = 0; }},
        {"zero TCP receive buffer",
         "tcp.receive_buffer_bytes",
         "--tcp-receive-buffer-bytes",
         [](auto& c) { c.tcp.receiveBufferBytes = 0; }},
        {"segment exceeds send buffer",
         "tcp.segment_size_bytes",
         "--tcp-segment-size-bytes",
         [](auto& c) { c.tcp.sendBufferBytes = c.tcp.segmentSizeBytes - 1; }},
        {"segment exceeds receive buffer",
         "tcp.segment_size_bytes",
         "--tcp-segment-size-bytes",
         [](auto& c) { c.tcp.receiveBufferBytes = c.tcp.segmentSizeBytes - 1; }},
        {"zero statistics window",
         "statistics.window_ms",
         "--statistics-window-ms",
         [](auto& c) { c.statistics.windowMs = 0; }},
        {"invalid scenario log",
         "logging.sample_scenario_level",
         "--logging-sample-scenario-level",
         [](auto& c) { c.logging.sampleScenarioLevel = "verbose"; }},
        {"invalid AP log",
         "logging.ap_generator_level",
         "--logging-ap-generator-level",
         [](auto& c) { c.logging.apGeneratorLevel = "verbose"; }},
        {"invalid station log",
         "logging.sta_generator_level",
         "--logging-sta-generator-level",
         [](auto& c) { c.logging.staGeneratorLevel = "verbose"; }},
        {"invalid sink log",
         "logging.traffic_sink_level",
         "--logging-traffic-sink-level",
         [](auto& c) { c.logging.trafficSinkLevel = "verbose"; }},
        {"invalid distribution log",
         "logging.contention_distribution_level",
         "--logging-contention-distribution-level",
         [](auto& c) { c.logging.contentionDistributionLevel = "verbose"; }},
    };

    for (const auto& testCase : invalidCases)
    {
        CheckInvalid(testCase);
    }

    auto exactError = MakeValidConfig();
    exactError.wifi.bandwidthMhz = 30;
    try
    {
        ValidateScenarioConfig(exactError);
        NS_TEST_ASSERT_MSG_EQ(true, false, "Invalid bandwidth accepted");
    }
    catch (const ScenarioConfigError& error)
    {
        NS_TEST_ASSERT_MSG_EQ(std::string(error.what()),
                              "invalid wifi.bandwidth_mhz (--wifi-bandwidth-mhz): expected 20, 40, "
                              "80, or 160; got 30",
                              "Wrong canonical validation error");
    }
}

/**
 * @ingroup tests
 *
 * Verify positive validation boundaries and runtime conversions.
 */
class ScenarioConfigConversionsTestCase : public TestCase
{
  public:
    ScenarioConfigConversionsTestCase();

  private:
    void DoRun() override;
};

ScenarioConfigConversionsTestCase::ScenarioConfigConversionsTestCase()
    : TestCase("scenario configuration runtime conversions")
{
}

void
ScenarioConfigConversionsTestCase::DoRun()
{
    const std::vector<std::pair<WifiBandConfig, WifiPhyBand>> bands{
        {WifiBandConfig::BAND_2_4_GHZ, WIFI_PHY_BAND_2_4GHZ},
        {WifiBandConfig::BAND_5_GHZ, WIFI_PHY_BAND_5GHZ},
        {WifiBandConfig::BAND_6_GHZ, WIFI_PHY_BAND_6GHZ},
    };
    for (const auto& [configured, expected] : bands)
    {
        auto config = MakeValidConfig();
        config.wifi.band = configured;
        ValidateScenarioConfig(config);
        NS_TEST_ASSERT_MSG_EQ(ToWifiPhyBand(configured), expected, "Wrong Wi-Fi band conversion");
    }

    for (const int width : {20, 40, 80, 160})
    {
        auto config = MakeValidConfig();
        config.wifi.bandwidthMhz = width;
        config.wifi.primary20Index = static_cast<uint8_t>(width / 20 - 1);
        ValidateScenarioConfig(config);
    }

    struct LogLevelCase
    {
        std::string_view configured;
        std::optional<LogLevel> expected;
    };

    const std::vector<LogLevelCase> logLevels{
        {"off", std::nullopt},
        {"error", LOG_LEVEL_ERROR},
        {"warn", LOG_LEVEL_WARN},
        {"info", LOG_LEVEL_INFO},
        {"debug", LOG_LEVEL_DEBUG},
        {"function", LOG_LEVEL_FUNCTION},
        {"logic", LOG_LEVEL_LOGIC},
        {"all", LOG_LEVEL_ALL},
    };
    for (const auto& testCase : logLevels)
    {
        auto config = MakeValidConfig();
        config.logging.sampleScenarioLevel = testCase.configured;
        config.logging.apGeneratorLevel = testCase.configured;
        config.logging.staGeneratorLevel = testCase.configured;
        config.logging.trafficSinkLevel = testCase.configured;
        config.logging.contentionDistributionLevel = testCase.configured;
        ValidateScenarioConfig(config);

        const auto actual = ParseScenarioLogLevel(testCase.configured);
        NS_TEST_ASSERT_MSG_EQ(actual.has_value(),
                              testCase.expected.has_value(),
                              "Wrong optional state for " << testCase.configured);
        if (actual && testCase.expected)
        {
            NS_TEST_ASSERT_MSG_EQ(*actual,
                                  *testCase.expected,
                                  "Wrong level for " << testCase.configured);
        }
    }

    auto unlimited = MakeValidConfig();
    unlimited.distribution.maxAgentsPerStation = 0;
    ValidateScenarioConfig(unlimited);

    auto fixed = MakeValidConfig();
    fixed.simulation.durationMode = DurationMode::FIXED;
    fixed.simulation.fixedDurationSeconds = 0.001;
    ValidateScenarioConfig(fixed);

    auto maximumBounds = MakeValidConfig();
    maximumBounds.simulation.rngSeed = 4294944442U;
    maximumBounds.topology.bssCount = 256;
    maximumBounds.topology.stationsPerBss = 253;
    maximumBounds.topology.ssidPrefix = std::string(29, 's');
    ValidateScenarioConfig(maximumBounds);

    auto oneBssSsidBoundary = MakeValidConfig();
    oneBssSsidBoundary.topology.bssCount = 1;
    oneBssSsidBoundary.topology.ssidPrefix = std::string(31, 's');
    ValidateScenarioConfig(oneBssSsidBoundary);

    NS_TEST_ASSERT_MSG_EQ(ResolveWifiManagerType("ns3::MinstrelHtWifiManager").GetName(),
                          "ns3::MinstrelHtWifiManager",
                          "Wrong Wi-Fi manager TypeId");
    NS_TEST_ASSERT_MSG_EQ(ResolveTcpCongestionType("ns3::TcpHighSpeed").GetName(),
                          "ns3::TcpHighSpeed",
                          "Wrong TCP congestion TypeId");
}

/**
 * @ingroup tests
 *
 * Verify non-finite TOML and CLI values reach post-merge validation.
 */
class ScenarioConfigNonFiniteSourceTestCase : public TestCase
{
  public:
    ScenarioConfigNonFiniteSourceTestCase();

  private:
    void DoRun() override;
    void CheckValidationFailure(const ScenarioConfig& config, std::string_view source);
};

ScenarioConfigNonFiniteSourceTestCase::ScenarioConfigNonFiniteSourceTestCase()
    : TestCase("post-merge validation rejects non-finite values from TOML and CLI")
{
}

void
ScenarioConfigNonFiniteSourceTestCase::CheckValidationFailure(const ScenarioConfig& config,
                                                              std::string_view source)
{
    try
    {
        ValidateScenarioConfig(config);
        NS_TEST_ASSERT_MSG_EQ(true, false, "Non-finite value accepted from " << source);
    }
    catch (const ScenarioConfigError& error)
    {
        const std::string message = error.what();
        NS_TEST_ASSERT_MSG_NE(message.find("simulation.auto_tail_seconds"),
                              std::string::npos,
                              "Wrong non-finite error from " << source << ": " << message);
        NS_TEST_ASSERT_MSG_NE(message.find("--simulation-auto-tail-seconds"),
                              std::string::npos,
                              "Missing CLI flag for " << source << ": " << message);
    }
}

void
ScenarioConfigNonFiniteSourceTestCase::DoRun()
{
    for (const std::string_view value : {"nan", "inf"})
    {
        const auto path = CreateTempDirFilename("scenario-non-finite.toml");
        std::ofstream output(path);
        output << "[general]\ntrace_file = \"trace.json\"\n"
               << "[simulation]\nauto_tail_seconds = " << value << '\n';
        output.close();
        CheckValidationFailure(LoadTomlConfig(path), "TOML");
    }

    const std::filesystem::path configFile = CreateTempDirFilename("scenario-cli-non-finite.toml");
    std::ofstream output(configFile);
    output << "[general]\ntrace_file = \"trace.json\"\n";
    output.close();
    for (const std::string value : {"nan", "inf"})
    {
        const auto result = ParseScenarioArguments(
            {"--config", configFile.filename().string(), "--simulation-auto-tail-seconds", value},
            configFile.parent_path());
        NS_TEST_ASSERT_MSG_EQ(result.valid,
                              true,
                              "CLI parser rejected " << value << ": " << result.error);
        CheckValidationFailure(result.launch.scenario, "CLI");
    }
}

} // namespace

std::vector<TestCase*>
CreateScenarioConfigValidationTestCases()
{
    return {new ScenarioConfigValidationTestCase,
            new ScenarioConfigConversionsTestCase,
            new ScenarioConfigNonFiniteSourceTestCase};
}
