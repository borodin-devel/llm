#include "../../examples/saturated-tcp/config.h"
#include "../../examples/statistics/json/writer.h"
#include "../llm-test-suite.h"

#include "ns3/json.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace ns3;

namespace
{

std::filesystem::path
WriteConfigFixture(const std::filesystem::path& path, std::string_view contents)
{
    std::ofstream output(path);
    output << contents;
    output.close();
    return path;
}

SaturatedTcpConfig
ParseConfig(const std::vector<std::string>& arguments)
{
    std::vector<std::string> storage{"saturated-tcp-scenario"};
    storage.insert(storage.end(), arguments.begin(), arguments.end());
    std::vector<char*> argv;
    argv.reserve(storage.size());
    for (auto& argument : storage)
    {
        argv.push_back(argument.data());
    }
    return ParseSaturatedTcpConfig(static_cast<int>(argv.size()), argv.data());
}

std::string
WriteConfiguration(const SaturatedTcpConfig& config)
{
    std::ostringstream output;
    JsonWriter writer(output);
    WriteEffectiveSaturatedTcpConfigurationJson(writer, config);
    writer.Finish();
    return output.str();
}

std::vector<std::string>
GetKeys(const nlohmann::ordered_json& object)
{
    std::vector<std::string> actual;
    for (const auto& [key, value] : object.items())
    {
        static_cast<void>(value);
        actual.push_back(key);
    }
    return actual;
}

/**
 * @ingroup tests
 *
 * Verify all compiled saturated benchmark defaults.
 */
class SaturatedTcpConfigDefaultsTestCase : public TestCase
{
  public:
    /** Construct the default-value test. */
    SaturatedTcpConfigDefaultsTestCase();

  private:
    void DoRun() override;
};

SaturatedTcpConfigDefaultsTestCase::SaturatedTcpConfigDefaultsTestCase()
    : TestCase("saturated TCP configuration defaults")
{
}

void
SaturatedTcpConfigDefaultsTestCase::DoRun()
{
    const SaturatedTcpConfig config;
    NS_TEST_ASSERT_MSG_EQ(config.general.outputName, "output.json", "Wrong output name");
    NS_TEST_ASSERT_MSG_EQ(config.general.runFolder.has_value(),
                          false,
                          "Run folder is not optional");
    NS_TEST_ASSERT_MSG_EQ(config.script.repetitions, 1, "Wrong repetition count");
    NS_TEST_ASSERT_MSG_EQ(config.simulation.rngSeed, 12345, "Wrong RNG seed");
    NS_TEST_ASSERT_MSG_EQ(config.simulation.rngRun, 1, "Wrong RNG run");
    NS_TEST_ASSERT_MSG_EQ(config.benchmark.stationCountPerBss, 5, "Wrong station count");
    NS_TEST_ASSERT_MSG_EQ(config.benchmark.rssiRange, SaturatedRssiRange::HIGH, "Wrong RSSI range");
    NS_TEST_ASSERT_MSG_EQ(config.benchmark.interferenceMode,
                          SaturatedInterferenceMode::ISOLATED,
                          "Wrong interference mode");
    NS_TEST_ASSERT_MSG_EQ(config.benchmark.trafficMode,
                          SaturatedTrafficMode::UL,
                          "Wrong traffic mode");
    NS_TEST_ASSERT_MSG_EQ(config.benchmark.mimoMode, SaturatedMimoMode::SU, "Wrong MIMO mode");
    NS_TEST_ASSERT_MSG_EQ(config.wifi.band, "5GHz", "Wrong band");
    NS_TEST_ASSERT_MSG_EQ(config.wifi.channelNumber, 42, "Wrong channel number");
    NS_TEST_ASSERT_MSG_EQ(config.wifi.bandwidthMhz, 80, "Wrong bandwidth");
    NS_TEST_ASSERT_MSG_EQ(config.wifi.primary20Index, 0, "Wrong primary index");
    NS_TEST_ASSERT_MSG_EQ_TOL(config.wifi.txPowerDbm, 20.0, 1e-12, "Wrong TX power");
    NS_TEST_ASSERT_MSG_EQ(config.wifi.rateManager,
                          "ns3::MinstrelHtWifiManager",
                          "Wrong rate manager");
    NS_TEST_ASSERT_MSG_EQ(config.wifi.antennas, 2, "Wrong antenna count");
    NS_TEST_ASSERT_MSG_EQ(config.wifi.maxTxSpatialStreams, 2, "Wrong maximum TX streams");
    NS_TEST_ASSERT_MSG_EQ(config.wifi.maxRxSpatialStreams, 2, "Wrong maximum RX streams");
    NS_TEST_ASSERT_MSG_EQ(config.tcp.congestionControl,
                          "ns3::TcpHighSpeed",
                          "Wrong congestion control");
    NS_TEST_ASSERT_MSG_EQ(config.tcp.segmentSizeBytes, 1460, "Wrong segment size");
    NS_TEST_ASSERT_MSG_EQ(config.tcp.sendBufferBytes, 33554432, "Wrong send buffer");
    NS_TEST_ASSERT_MSG_EQ(config.tcp.receiveBufferBytes, 33554432, "Wrong receive buffer");
    NS_TEST_ASSERT_MSG_EQ(config.tcp.wiredRate, "10Gbps", "Wrong wired rate");
    NS_TEST_ASSERT_MSG_EQ(config.tcp.wiredDelay, "0.1ms", "Wrong wired delay");
    NS_TEST_ASSERT_MSG_EQ(config.statistics.windowMs, 10, "Wrong statistics window");
    NS_TEST_ASSERT_MSG_EQ(config.logging.scenarioLevel, "info", "Wrong logging level");
}

/**
 * @ingroup tests
 *
 * Verify strict TOML parsing, exact enums, and CLI precedence.
 */
class SaturatedTcpConfigParsingTestCase : public TestCase
{
  public:
    /** Construct the parser test. */
    SaturatedTcpConfigParsingTestCase();

  private:
    /** Run all parser checks. */
    void DoRun() override;

    /**
     * Require parsing to fail with a diagnostic fragment.
     *
     * @param arguments Command-line arguments excluding the executable name.
     * @param expected Expected diagnostic fragment.
     */
    void CheckFailure(const std::vector<std::string>& arguments, std::string_view expected);
};

SaturatedTcpConfigParsingTestCase::SaturatedTcpConfigParsingTestCase()
    : TestCase("saturated TCP TOML and CLI parsing")
{
}

void
SaturatedTcpConfigParsingTestCase::CheckFailure(const std::vector<std::string>& arguments,
                                                std::string_view expected)
{
    try
    {
        static_cast<void>(ParseConfig(arguments));
        NS_TEST_ASSERT_MSG_EQ(true, false, "Invalid saturated configuration accepted");
    }
    catch (const SaturatedTcpConfigError& error)
    {
        const std::string message = error.what();
        NS_TEST_ASSERT_MSG_NE(message.find(expected),
                              std::string::npos,
                              "Wrong saturated diagnostic: " << message);
    }
}

void
SaturatedTcpConfigParsingTestCase::DoRun()
{
    const auto configPath = WriteConfigFixture(CreateTempDirFilename("saturated-overrides.toml"),
                                               "[script]\n"
                                               "repetitions = 7\n"
                                               "[simulation]\n"
                                               "rng_seed = 22\n"
                                               "rng_run = 3\n"
                                               "[benchmark]\n"
                                               "sta_count_per_bss = 12\n"
                                               "rssi_range = \"medium\"\n"
                                               "interference_mode = \"ap_only_cochannel\"\n"
                                               "traffic_mode = \"dl\"\n"
                                               "mimo_mode = \"su\"\n"
                                               "[wifi]\n"
                                               "tx_power_dbm = 20.0\n"
                                               "[statistics]\n"
                                               "window_ms = 20\n");
    const auto config = ParseConfig({"--benchmark-rssi-range=low",
                                     "--benchmark-traffic-mode",
                                     "ul_dl",
                                     "--benchmark-sta-count-per-bss",
                                     "30",
                                     "--simulation-rng-run=9",
                                     "--config",
                                     configPath.string()});
    NS_TEST_ASSERT_MSG_EQ(config.script.repetitions, 7, "TOML repetition not loaded");
    NS_TEST_ASSERT_MSG_EQ(config.simulation.rngSeed, 22, "TOML seed not loaded");
    NS_TEST_ASSERT_MSG_EQ(config.simulation.rngRun, 9, "Equals override not applied");
    NS_TEST_ASSERT_MSG_EQ(config.benchmark.stationCountPerBss, 30, "CLI station override lost");
    NS_TEST_ASSERT_MSG_EQ(config.benchmark.rssiRange, SaturatedRssiRange::LOW, "Wrong RSSI enum");
    NS_TEST_ASSERT_MSG_EQ(config.benchmark.interferenceMode,
                          SaturatedInterferenceMode::AP_ONLY_COCHANNEL,
                          "Wrong interference enum");
    NS_TEST_ASSERT_MSG_EQ(config.benchmark.trafficMode,
                          SaturatedTrafficMode::UL_DL,
                          "Wrong traffic enum");
    NS_TEST_ASSERT_MSG_EQ_TOL(config.wifi.txPowerDbm, 20.0, 1e-12, "TOML float not loaded");
    NS_TEST_ASSERT_MSG_EQ(config.statistics.windowMs, 20, "TOML integer not loaded");

    const std::vector<std::pair<std::string, SaturatedRssiRange>> rssiValues{
        {"high", SaturatedRssiRange::HIGH},
        {"medium", SaturatedRssiRange::MEDIUM},
        {"low", SaturatedRssiRange::LOW},
    };
    for (const auto& [text, expected] : rssiValues)
    {
        const auto parsed =
            ParseConfig({"--config", configPath.string(), "--benchmark-rssi-range", text});
        NS_TEST_ASSERT_MSG_EQ(parsed.benchmark.rssiRange,
                              expected,
                              "RSSI spelling did not round trip");
    }

    const std::vector<std::pair<std::string, SaturatedTrafficMode>> trafficValues{
        {"ul", SaturatedTrafficMode::UL},
        {"dl", SaturatedTrafficMode::DL},
        {"ul_dl", SaturatedTrafficMode::UL_DL},
    };
    for (const auto& [text, expected] : trafficValues)
    {
        const auto parsed =
            ParseConfig({"--config", configPath.string(), "--benchmark-traffic-mode", text});
        NS_TEST_ASSERT_MSG_EQ(parsed.benchmark.trafficMode,
                              expected,
                              "Traffic spelling did not round trip");
    }

    CheckFailure({}, "--config");
    CheckFailure({"--config", configPath.string(), "--benchmark-rssi-range", "HIGH"},
                 "benchmark.rssi_range");
    CheckFailure({"--config", configPath.string(), "--benchmark-mimo-mode", "mu"},
                 "DL MU-MIMO is not supported");

    const auto overriddenInvalidPath =
        WriteConfigFixture(CreateTempDirFilename("saturated-overridden-invalid.toml"),
                           "[benchmark]\nsta_count_per_bss = 0\n");
    const auto overridden = ParseConfig(
        {"--config", overriddenInvalidPath.string(), "--benchmark-sta-count-per-bss=1"});
    NS_TEST_ASSERT_MSG_EQ(overridden.benchmark.stationCountPerBss,
                          1,
                          "Validation ran before CLI precedence was resolved");

    const auto tracePath = WriteConfigFixture(CreateTempDirFilename("saturated-trace-option.toml"),
                                              "[general]\ntrace_file = \"trace.json\"\n");
    CheckFailure({"--config", tracePath.string()}, "unknown saturated TOML field");
    CheckFailure({"--config", configPath.string(), "--distribution-slot-ms", "10"},
                 "unknown saturated flag");
    CheckFailure({"--config", configPath.string(), "--general-run-folder", "--bogus"},
                 "unknown saturated flag: --bogus");
    CheckFailure({"--config", configPath.string(), "--bogus=value"},
                 "unknown saturated flag: --bogus");
    CheckFailure({"--config",
                  configPath.string(),
                  "--general-output-name",
                  "--benchmark-rssi-range",
                  "high"},
                 "saturated flag --general-output-name requires a value");
    CheckFailure({"--config", configPath.string(), "--wifi-tx-power-dbm", "-1"},
                 "wifi.tx_power_dbm");
    const auto optionLikeString =
        ParseConfig({"--config", configPath.string(), "--general-run-folder=--literal"});
    NS_TEST_ASSERT_MSG_EQ(optionLikeString.general.runFolder.value(),
                          "--literal",
                          "Equals syntax did not preserve option-like string value");
    const auto wrongFloatPath =
        WriteConfigFixture(CreateTempDirFilename("saturated-wrong-float.toml"),
                           "[wifi]\ntx_power_dbm = 20\n");
    CheckFailure({"--config", wrongFloatPath.string()}, "wifi.tx_power_dbm");
    const auto overflowPath = WriteConfigFixture(CreateTempDirFilename("saturated-overflow.toml"),
                                                 "[statistics]\nwindow_ms = 4294967296\n");
    CheckFailure({"--config", overflowPath.string()}, "statistics.window_ms");
}

/**
 * @ingroup tests
 *
 * Verify saturated configuration validation boundaries and diagnostics.
 */
class SaturatedTcpConfigValidationTestCase : public TestCase
{
  public:
    /** Construct the validation test. */
    SaturatedTcpConfigValidationTestCase();

  private:
    /** Run all validation checks. */
    void DoRun() override;

    /**
     * Require one invalid mutation to fail.
     *
     * @param mutate Mutation applied to an otherwise valid configuration.
     * @param expected Expected diagnostic fragment.
     */
    void CheckFailure(const std::function<void(SaturatedTcpConfig&)>& mutate,
                      std::string_view expected);

    /**
     * Require one valid ns-3 quantity mutation to pass.
     *
     * @param mutate Mutation applied to an otherwise valid configuration.
     * @param description Quantity spelling used in diagnostics.
     */
    void CheckValid(const std::function<void(SaturatedTcpConfig&)>& mutate,
                    std::string_view description);
};

SaturatedTcpConfigValidationTestCase::SaturatedTcpConfigValidationTestCase()
    : TestCase("saturated TCP configuration validation")
{
}

void
SaturatedTcpConfigValidationTestCase::CheckValid(
    const std::function<void(SaturatedTcpConfig&)>& mutate,
    std::string_view description)
{
    SaturatedTcpConfig config;
    mutate(config);
    try
    {
        ValidateSaturatedTcpConfig(config);
    }
    catch (const SaturatedTcpConfigError& error)
    {
        NS_TEST_ASSERT_MSG_EQ(true,
                              false,
                              "Valid ns-3 quantity rejected for " << description << ": "
                                                                  << error.what());
    }
}

void
SaturatedTcpConfigValidationTestCase::CheckFailure(
    const std::function<void(SaturatedTcpConfig&)>& mutate,
    std::string_view expected)
{
    SaturatedTcpConfig config;
    mutate(config);
    try
    {
        ValidateSaturatedTcpConfig(config);
        NS_TEST_ASSERT_MSG_EQ(true, false, "Invalid saturated configuration accepted");
    }
    catch (const SaturatedTcpConfigError& error)
    {
        const std::string message = error.what();
        NS_TEST_ASSERT_MSG_NE(message.find(expected),
                              std::string::npos,
                              "Wrong saturated validation error: " << message);
    }
}

void
SaturatedTcpConfigValidationTestCase::DoRun()
{
    for (const uint32_t stationCount : {1U, 30U})
    {
        SaturatedTcpConfig config;
        config.benchmark.stationCountPerBss = stationCount;
        ValidateSaturatedTcpConfig(config);
    }
    for (const uint32_t windowMs : {1U, 1000U})
    {
        SaturatedTcpConfig config;
        config.statistics.windowMs = windowMs;
        ValidateSaturatedTcpConfig(config);
    }
    for (const uint32_t seed : {1U, 4294944442U})
    {
        SaturatedTcpConfig config;
        config.simulation.rngSeed = seed;
        ValidateSaturatedTcpConfig(config);
    }

    CheckFailure([](auto& c) { c.script.repetitions = 0; }, "script.repetitions");
    CheckFailure([](auto& c) { c.simulation.rngSeed = 0; }, "simulation.rng_seed");
    CheckFailure([](auto& c) { c.simulation.rngSeed = 4294944443U; }, "simulation.rng_seed");
    CheckFailure([](auto& c) { c.simulation.rngRun = 0; }, "simulation.rng_run");
    CheckFailure([](auto& c) { c.benchmark.stationCountPerBss = 0; },
                 "benchmark.sta_count_per_bss");
    CheckFailure([](auto& c) { c.benchmark.stationCountPerBss = 31; },
                 "benchmark.sta_count_per_bss");
    CheckFailure([](auto& c) { c.benchmark.mimoMode = SaturatedMimoMode::MU; },
                 "DL MU-MIMO is not supported");
    CheckFailure([](auto& c) { c.general.runFolder = ""; }, "general.run_folder");
    CheckFailure([](auto& c) { c.general.runFolder = std::string("run\0hidden", 10); },
                 "general.run_folder");
    CheckFailure([](auto& c) { c.general.outputName = ""; }, "general.output_name");
    CheckFailure([](auto& c) { c.general.outputName = std::string("safe\0.json", 10); },
                 "general.output_name");
    CheckFailure([](auto& c) { c.general.outputName = "nested/output.json"; },
                 "general.output_name");
    CheckFailure([](auto& c) { c.general.outputName = "..json"; }, "general.output_name");
    CheckFailure([](auto& c) { c.general.outputName = "output.txt"; }, "general.output_name");
    CheckFailure([](auto& c) { c.wifi.txPowerDbm = 0.0; }, "wifi.tx_power_dbm");
    CheckFailure([](auto& c) { c.wifi.txPowerDbm = 17.5; }, "wifi.tx_power_dbm");
    CheckFailure([](auto& c) { c.wifi.txPowerDbm = std::numeric_limits<double>::infinity(); },
                 "wifi.tx_power_dbm");
    CheckFailure([](auto& c) { c.wifi.txPowerDbm = std::numeric_limits<double>::quiet_NaN(); },
                 "wifi.tx_power_dbm");
    CheckFailure([](auto& c) { c.wifi.band = "6GHz"; }, "wifi.band");
    CheckFailure([](auto& c) { c.wifi.channelNumber = 43; }, "wifi.channel_number");
    CheckFailure([](auto& c) { c.wifi.bandwidthMhz = 40; }, "wifi.bandwidth_mhz");
    CheckFailure([](auto& c) { c.wifi.primary20Index = 1; }, "wifi.primary_20_index");
    CheckFailure([](auto& c) { c.wifi.antennas = 0; }, "wifi.antennas");
    CheckFailure([](auto& c) { c.wifi.maxTxSpatialStreams = 3; }, "wifi.max_tx_spatial_streams");
    CheckFailure([](auto& c) { c.wifi.maxRxSpatialStreams = 3; }, "wifi.max_rx_spatial_streams");
    CheckFailure([](auto& c) { c.tcp.segmentSizeBytes = 0; }, "tcp.segment_size_bytes");
    CheckFailure([](auto& c) { c.tcp.sendBufferBytes = 0; }, "tcp.send_buffer_bytes");
    CheckFailure([](auto& c) { c.tcp.receiveBufferBytes = 0; }, "tcp.receive_buffer_bytes");
    CheckFailure([](auto& c) { c.tcp.sendBufferBytes = c.tcp.segmentSizeBytes - 1; },
                 "tcp.segment_size_bytes");
    CheckFailure([](auto& c) { c.tcp.receiveBufferBytes = c.tcp.segmentSizeBytes - 1; },
                 "tcp.segment_size_bytes");
    CheckFailure([](auto& c) { c.tcp.wiredRate = "fast"; }, "tcp.wired_rate");
    CheckFailure([](auto& c) { c.tcp.wiredRate = "."; }, "tcp.wired_rate");
    CheckFailure([](auto& c) { c.tcp.wiredRate = " "; }, "tcp.wired_rate");
    CheckFailure([](auto& c) { c.tcp.wiredRate = "Gbps"; }, "tcp.wired_rate");
    CheckFailure([](auto& c) { c.tcp.wiredRate = "0Gbps"; }, "tcp.wired_rate");
    CheckFailure([](auto& c) { c.tcp.wiredRate = "1e3Gbps"; }, "tcp.wired_rate");
    CheckFailure([](auto& c) { c.tcp.wiredRate = "1..2Gbps"; }, "tcp.wired_rate");
    CheckFailure([](auto& c) { c.tcp.wiredRate = "1 2Gbps"; }, "tcp.wired_rate");
    CheckFailure([](auto& c) { c.tcp.wiredRate = "1.5bps"; }, "tcp.wired_rate");
    CheckFailure([](auto& c) { c.tcp.wiredRate = "1.5"; }, "tcp.wired_rate");
    CheckFailure([](auto& c) { c.tcp.wiredDelay = "soon"; }, "tcp.wired_delay");
    CheckFailure([](auto& c) { c.tcp.wiredDelay = "0ms"; }, "tcp.wired_delay");
    CheckFailure([](auto& c) { c.tcp.wiredDelay = "1 ms"; }, "tcp.wired_delay");
    CheckValid([](auto& c) { c.tcp.wiredRate = "1Gib/s"; }, "1Gib/s");
    CheckValid([](auto& c) { c.tcp.wiredRate = "1GiB/s"; }, "1GiB/s");
    CheckValid([](auto& c) { c.tcp.wiredRate = "1.5Kbps"; }, "1.5Kbps");
    CheckValid([](auto& c) { c.tcp.wiredDelay = "+0.1ms"; }, "+0.1ms");
    CheckValid([](auto& c) { c.tcp.wiredDelay = "1y"; }, "1y");
    CheckFailure([](auto& c) { c.statistics.windowMs = 0; }, "statistics.window_ms");
    CheckFailure([](auto& c) { c.statistics.windowMs = 3; }, "divide 1000");
    CheckFailure([](auto& c) { c.statistics.windowMs = 1001; }, "divide 1000");
    CheckFailure([](auto& c) { c.wifi.rateManager = "ns3::MissingWifiManager"; },
                 "wifi.rate_manager");
    CheckFailure([](auto& c) { c.wifi.rateManager = "ns3::TcpHighSpeed"; }, "wifi.rate_manager");
    CheckFailure([](auto& c) { c.wifi.rateManager = "ns3::ConstantRateWifiManager"; },
                 "wifi.rate_manager");
    CheckFailure([](auto& c) { c.tcp.congestionControl = "ns3::MissingTcpType"; },
                 "tcp.congestion_control");
    CheckFailure([](auto& c) { c.tcp.congestionControl = "ns3::MinstrelHtWifiManager"; },
                 "tcp.congestion_control");
    CheckFailure([](auto& c) { c.tcp.congestionControl = "ns3::TcpCubic"; },
                 "tcp.congestion_control");
    CheckFailure([](auto& c) { c.logging.scenarioLevel = "verbose"; }, "logging.scenario_level");
    CheckFailure([](auto& c) { c.benchmark.rssiRange = static_cast<SaturatedRssiRange>(99); },
                 "benchmark.rssi_range");
    CheckFailure(
        [](auto& c) { c.benchmark.interferenceMode = static_cast<SaturatedInterferenceMode>(99); },
        "benchmark.interference_mode");
    CheckFailure([](auto& c) { c.benchmark.trafficMode = static_cast<SaturatedTrafficMode>(99); },
                 "benchmark.traffic_mode");
}

/**
 * @ingroup tests
 *
 * Verify effective JSON section order, key order, spellings, and scalar types.
 */
class SaturatedTcpConfigJsonTestCase : public TestCase
{
  public:
    /** Construct the JSON metadata test. */
    SaturatedTcpConfigJsonTestCase();

  private:
    /**
     * Verify an object's exact member order.
     *
     * @param object Object whose keys are inspected.
     * @param expected Expected key sequence.
     * @param objectName Object name used in diagnostics.
     */
    void CheckKeys(const nlohmann::ordered_json& object,
                   const std::vector<std::string>& expected,
                   std::string_view objectName);

    void DoRun() override;
};

SaturatedTcpConfigJsonTestCase::SaturatedTcpConfigJsonTestCase()
    : TestCase("saturated TCP effective configuration JSON")
{
}

void
SaturatedTcpConfigJsonTestCase::CheckKeys(const nlohmann::ordered_json& object,
                                          const std::vector<std::string>& expected,
                                          std::string_view objectName)
{
    const auto actual = GetKeys(object);
    NS_TEST_ASSERT_MSG_EQ(actual.size(), expected.size(), "Wrong key count in " << objectName);
    if (actual.size() != expected.size())
    {
        return;
    }
    for (std::size_t index = 0; index < expected.size(); ++index)
    {
        NS_TEST_ASSERT_MSG_EQ(actual[index],
                              expected[index],
                              "Wrong key at index " << index << " in " << objectName);
    }
}

void
SaturatedTcpConfigJsonTestCase::DoRun()
{
    SaturatedTcpConfig config;
    config.general.runFolder = "run/attempt-1";
    config.benchmark.rssiRange = SaturatedRssiRange::MEDIUM;
    config.benchmark.interferenceMode = SaturatedInterferenceMode::AP_ONLY_COCHANNEL;
    config.benchmark.trafficMode = SaturatedTrafficMode::UL_DL;

    const auto text = WriteConfiguration(config);
    const auto document = nlohmann::ordered_json::parse(text);
    const std::vector<std::string> expectedSections{"general",
                                                    "script",
                                                    "simulation",
                                                    "benchmark",
                                                    "wifi",
                                                    "tcp",
                                                    "statistics",
                                                    "logging"};
    CheckKeys(document, expectedSections, "root");

    CheckKeys(document.at("general"), {"output_name", "run_folder"}, "general configuration");
    CheckKeys(document.at("script"), {"repetitions"}, "script configuration");
    CheckKeys(document.at("simulation"), {"rng_seed", "rng_run"}, "simulation configuration");
    CheckKeys(document.at("benchmark"),
              {"sta_count_per_bss", "rssi_range", "interference_mode", "traffic_mode", "mimo_mode"},
              "benchmark configuration");

    const std::vector<std::string> expectedWifiKeys{"band",
                                                    "channel_number",
                                                    "bandwidth_mhz",
                                                    "primary_20_index",
                                                    "tx_power_dbm",
                                                    "rate_manager",
                                                    "antennas",
                                                    "max_tx_spatial_streams",
                                                    "max_rx_spatial_streams"};
    CheckKeys(document.at("wifi"), expectedWifiKeys, "Wi-Fi configuration");
    CheckKeys(document.at("tcp"),
              {"congestion_control",
               "segment_size_bytes",
               "send_buffer_bytes",
               "receive_buffer_bytes",
               "wired_rate",
               "wired_delay"},
              "TCP configuration");
    CheckKeys(document.at("statistics"), {"window_ms"}, "statistics configuration");
    CheckKeys(document.at("logging"), {"scenario_level"}, "logging configuration");
    NS_TEST_ASSERT_MSG_EQ(document.at("general").at("run_folder").is_string(),
                          true,
                          "Run folder is not a string");
    NS_TEST_ASSERT_MSG_EQ(document.at("script").at("repetitions").is_number_unsigned(),
                          true,
                          "Repetitions is not unsigned integer metadata");
    NS_TEST_ASSERT_MSG_EQ(document.at("wifi").at("tx_power_dbm").is_number_float(),
                          true,
                          "TX power is not floating-point JSON");
    NS_TEST_ASSERT_MSG_EQ(document.at("benchmark").at("rssi_range"),
                          "medium",
                          "Wrong RSSI spelling");
    NS_TEST_ASSERT_MSG_EQ(document.at("benchmark").at("interference_mode"),
                          "ap_only_cochannel",
                          "Wrong interference spelling");
    NS_TEST_ASSERT_MSG_EQ(document.at("benchmark").at("traffic_mode"),
                          "ul_dl",
                          "Wrong traffic spelling");
    NS_TEST_ASSERT_MSG_EQ(document.at("benchmark").at("mimo_mode"), "su", "Wrong MIMO spelling");

    config.general.runFolder.reset();
    const auto withoutFolder = nlohmann::json::parse(WriteConfiguration(config));
    NS_TEST_ASSERT_MSG_EQ(withoutFolder.at("general").at("run_folder").is_null(),
                          true,
                          "Absent run folder is not JSON null");
}

} // namespace

std::vector<TestCase*>
CreateSaturatedTcpConfigTestCases()
{
    return {
        new SaturatedTcpConfigDefaultsTestCase,
        new SaturatedTcpConfigParsingTestCase,
        new SaturatedTcpConfigValidationTestCase,
        new SaturatedTcpConfigJsonTestCase,
    };
}
