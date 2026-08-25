#include "../examples/scenario-config.h"
#include "llm-test-suite.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

using namespace ns3;

namespace
{

/**
 * @ingroup tests
 *
 * Verify the typed scenario configuration defaults.
 */
class ScenarioConfigDefaultsTestCase : public TestCase
{
  public:
    ScenarioConfigDefaultsTestCase();

  private:
    void DoRun() override;
};

ScenarioConfigDefaultsTestCase::ScenarioConfigDefaultsTestCase()
    : TestCase("scenario configuration typed defaults")
{
}

void
ScenarioConfigDefaultsTestCase::DoRun()
{
    ScenarioConfig config;
    NS_TEST_ASSERT_MSG_EQ(config.general.traceFile.empty(), true, "Trace must have no default");
    NS_TEST_ASSERT_MSG_EQ(config.general.runFolder.has_value(), false, "Run folder must be optional");
    NS_TEST_ASSERT_MSG_EQ(config.general.outputName, "mac-node-stats.json", "Wrong output name");
    NS_TEST_ASSERT_MSG_EQ(config.simulation.durationMode, DurationMode::AUTO, "Wrong duration mode");
    NS_TEST_ASSERT_MSG_EQ(config.simulation.fixedDurationSeconds, 0.0, "Wrong fixed duration");
    NS_TEST_ASSERT_MSG_EQ(config.simulation.autoTailSeconds, 2.0, "Wrong tail");
    NS_TEST_ASSERT_MSG_EQ(config.simulation.rngSeed, 12345, "Wrong seed");
    NS_TEST_ASSERT_MSG_EQ(config.simulation.rngRun, 1, "Wrong run");
    NS_TEST_ASSERT_MSG_EQ(config.topology.bssCount, 3, "Wrong BSS count");
    NS_TEST_ASSERT_MSG_EQ(config.topology.stationsPerBss, 30, "Wrong STA count");
    NS_TEST_ASSERT_MSG_EQ(config.topology.bssSpacingM, 100.0, "Wrong spacing");
    NS_TEST_ASSERT_MSG_EQ(config.topology.stationRadiusM, 5.0, "Wrong radius");
    NS_TEST_ASSERT_MSG_EQ(config.topology.isolateBssChannels, true, "Wrong isolation");
    NS_TEST_ASSERT_MSG_EQ(config.topology.ssidPrefix, "llm-ap-", "Wrong SSID prefix");
    NS_TEST_ASSERT_MSG_EQ(config.topology.apSinkPort, 10000, "Wrong AP port");
    NS_TEST_ASSERT_MSG_EQ(config.topology.stationSinkBasePort, 9000, "Wrong STA port");
    NS_TEST_ASSERT_MSG_EQ(config.topology.generatorStartSeconds, 1.0, "Wrong start");
    NS_TEST_ASSERT_MSG_EQ(config.distribution.maxAgentsPerStation, 832, "Wrong cap");
    NS_TEST_ASSERT_MSG_EQ(config.distribution.lowContentionPriority, true, "Wrong policy");
    NS_TEST_ASSERT_MSG_EQ(config.distribution.slotMs, 10, "Wrong slot");
    NS_TEST_ASSERT_MSG_EQ(config.wifi.band, WifiBandConfig::BAND_5_GHZ, "Wrong band");
    NS_TEST_ASSERT_MSG_EQ(config.wifi.channelNumber, 0, "Wrong channel");
    NS_TEST_ASSERT_MSG_EQ(config.wifi.bandwidthMhz, 20, "Wrong bandwidth");
    NS_TEST_ASSERT_MSG_EQ(config.wifi.primary20Index, 0, "Wrong primary index");
    NS_TEST_ASSERT_MSG_EQ(config.wifi.rateManager, "ns3::MinstrelHtWifiManager", "Wrong manager");
    NS_TEST_ASSERT_MSG_EQ(config.wifi.activeProbing, true, "Wrong probing");
    NS_TEST_ASSERT_MSG_EQ(config.tcp.congestionControl, "ns3::TcpHighSpeed", "Wrong TCP type");
    NS_TEST_ASSERT_MSG_EQ(config.tcp.segmentSizeBytes, 1460, "Wrong segment");
    NS_TEST_ASSERT_MSG_EQ(config.tcp.sendBufferBytes, 33554432, "Wrong send buffer");
    NS_TEST_ASSERT_MSG_EQ(config.tcp.receiveBufferBytes, 33554432, "Wrong receive buffer");
    NS_TEST_ASSERT_MSG_EQ(config.statistics.windowMs, 10, "Wrong window");
    NS_TEST_ASSERT_MSG_EQ(config.logging.sampleScenarioLevel, "info", "Wrong scenario log");
    NS_TEST_ASSERT_MSG_EQ(config.logging.apGeneratorLevel, "warn", "Wrong AP log");
    NS_TEST_ASSERT_MSG_EQ(config.logging.staGeneratorLevel, "warn", "Wrong STA log");
    NS_TEST_ASSERT_MSG_EQ(config.logging.trafficSinkLevel, "warn", "Wrong sink log");
    NS_TEST_ASSERT_MSG_EQ(config.logging.contentionDistributionLevel,
                          "info",
                          "Wrong placement log");
}

/**
 * @ingroup tests
 *
 * Verify that the public option metadata is complete and unambiguous.
 */
class ScenarioConfigRegistryTestCase : public TestCase
{
  public:
    ScenarioConfigRegistryTestCase();

  private:
    void DoRun() override;
};

ScenarioConfigRegistryTestCase::ScenarioConfigRegistryTestCase()
    : TestCase("scenario configuration option registry")
{
}

void
ScenarioConfigRegistryTestCase::DoRun()
{
    const auto& options = GetScenarioConfigOptionInfo();
    NS_TEST_ASSERT_MSG_EQ(options.size(), 36, "Wrong option count");

    std::set<std::string> tomlPaths;
    std::set<std::string> cliFlags;
    std::map<std::string, std::size_t> sectionCounts;
    for (const auto& option : options)
    {
        NS_TEST_ASSERT_MSG_EQ(tomlPaths.insert(std::string(option.tomlPath)).second,
                              true,
                              "Duplicate TOML path " << option.tomlPath);
        NS_TEST_ASSERT_MSG_EQ(cliFlags.insert(std::string(option.cliFlag)).second,
                              true,
                              "Duplicate CLI flag " << option.cliFlag);

        std::string expectedFlag{"--"};
        expectedFlag.append(option.tomlPath);
        std::replace(expectedFlag.begin(), expectedFlag.end(), '.', '-');
        std::replace(expectedFlag.begin(), expectedFlag.end(), '_', '-');
        NS_TEST_ASSERT_MSG_EQ(option.cliFlag,
                              expectedFlag,
                              "CLI flag is not derived from " << option.tomlPath);

        const auto separator = option.tomlPath.find('.');
        NS_TEST_ASSERT_MSG_NE(separator, std::string_view::npos, "Option lacks a section");
        ++sectionCounts[std::string(option.tomlPath.substr(0, separator))];
    }

    const std::map<std::string, std::size_t> expectedCounts{{"general", 3},
                                                            {"simulation", 5},
                                                            {"topology", 9},
                                                            {"distribution", 3},
                                                            {"wifi", 6},
                                                            {"tcp", 4},
                                                            {"statistics", 1},
                                                            {"logging", 5}};
    NS_TEST_ASSERT_MSG_EQ(sectionCounts.size(), expectedCounts.size(), "Wrong section count");
    for (const auto& [section, count] : expectedCounts)
    {
        NS_TEST_ASSERT_MSG_EQ(sectionCounts[section],
                              count,
                              "Wrong option count for section " << section);
    }
}

/**
 * @ingroup tests
 *
 * Verify strict TOML loading and typed assignment.
 */
class ScenarioConfigTomlTestCase : public TestCase
{
  public:
    ScenarioConfigTomlTestCase();

  private:
    void DoRun() override;
    std::string WriteFixture(const std::string& name, std::string_view contents);
    void CheckFailure(const std::string& name,
                      std::string_view contents,
                      std::string_view expectedMessage);
};

ScenarioConfigTomlTestCase::ScenarioConfigTomlTestCase()
    : TestCase("strict scenario TOML loading")
{
}

std::string
ScenarioConfigTomlTestCase::WriteFixture(const std::string& name, std::string_view contents)
{
    const std::string path = CreateTempDirFilename(name);
    std::ofstream output(path);
    output << contents;
    output.close();
    return path;
}

void
ScenarioConfigTomlTestCase::CheckFailure(const std::string& name,
                                         std::string_view contents,
                                         std::string_view expectedMessage)
{
    const auto path = WriteFixture(name, contents);
    try
    {
        LoadTomlConfig(path);
        NS_TEST_ASSERT_MSG_EQ(true, false, "Invalid fixture accepted: " << name);
    }
    catch (const ScenarioConfigError& error)
    {
        const std::string message = error.what();
        NS_TEST_ASSERT_MSG_NE(message.find(expectedMessage),
                              std::string::npos,
                              "Wrong error for " << name << ": " << message);
    }
}

void
ScenarioConfigTomlTestCase::DoRun()
{
    const auto validPath = WriteFixture("scenario-valid.toml",
                                        "[general]\n"
                                        "trace_file = \"trace.json\"\n"
                                        "output_name = \"custom.json\"\n"
                                        "[simulation]\n"
                                        "duration_mode = \"fixed\"\n"
                                        "fixed_duration_seconds = 12.5\n"
                                        "[topology]\n"
                                        "bss_count = 4\n"
                                        "[distribution]\n"
                                        "low_contention_priority = false\n"
                                        "[wifi]\n"
                                        "band = \"6GHz\"\n"
                                        "bandwidth_mhz = 80\n"
                                        "[tcp]\n"
                                        "segment_size_bytes = 1200\n"
                                        "[statistics]\n"
                                        "window_ms = 25\n"
                                        "[logging]\n"
                                        "sample_scenario_level = \"debug\"\n");
    const auto config = LoadTomlConfig(validPath);
    NS_TEST_ASSERT_MSG_EQ(config.general.traceFile, "trace.json", "Wrong trace file");
    NS_TEST_ASSERT_MSG_EQ(config.general.outputName, "custom.json", "Wrong output name");
    NS_TEST_ASSERT_MSG_EQ(config.general.runFolder.has_value(), false, "Wrong run folder");
    NS_TEST_ASSERT_MSG_EQ(config.simulation.durationMode,
                          DurationMode::FIXED,
                          "Wrong duration mode");
    NS_TEST_ASSERT_MSG_EQ_TOL(config.simulation.fixedDurationSeconds,
                              12.5,
                              1e-12,
                              "Wrong fixed duration");
    NS_TEST_ASSERT_MSG_EQ_TOL(config.simulation.autoTailSeconds,
                              2.0,
                              1e-12,
                              "Omission did not preserve default");
    NS_TEST_ASSERT_MSG_EQ(config.topology.bssCount, 4, "Wrong BSS count");
    NS_TEST_ASSERT_MSG_EQ(config.distribution.lowContentionPriority,
                          false,
                          "Wrong contention policy");
    NS_TEST_ASSERT_MSG_EQ(config.wifi.band, WifiBandConfig::BAND_6_GHZ, "Wrong Wi-Fi band");
    NS_TEST_ASSERT_MSG_EQ(config.wifi.bandwidthMhz, 80, "Wrong bandwidth");
    NS_TEST_ASSERT_MSG_EQ(config.tcp.segmentSizeBytes, 1200, "Wrong segment size");
    NS_TEST_ASSERT_MSG_EQ(config.statistics.windowMs, 25, "Wrong statistics window");
    NS_TEST_ASSERT_MSG_EQ(config.logging.sampleScenarioLevel, "debug", "Wrong log level");

    CheckFailure("scenario-missing-general.toml",
                 "[wifi]\nbandwidth_mhz = 20\n",
                 "general.trace_file");
    CheckFailure("scenario-missing-trace.toml",
                 "[general]\noutput_name = \"out.json\"\n",
                 "general.trace_file");
    CheckFailure("scenario-empty-trace.toml", "[general]\ntrace_file = \"\"\n", "non-empty");
    CheckFailure("scenario-unknown-section.toml",
                 "[general]\ntrace_file = \"trace.json\"\n[unknown]\nvalue = 1\n",
                 "unknown");
    CheckFailure("scenario-unknown-field.toml",
                 "[general]\ntrace_file = \"trace.json\"\nunknown = true\n",
                 "general.unknown");
    CheckFailure("scenario-wrong-type.toml",
                 "[general]\ntrace_file = \"trace.json\"\n[topology]\nbss_count = \"4\"\n",
                 "topology.bss_count");
    CheckFailure("scenario-integer-overflow.toml",
                 "[general]\ntrace_file = \"trace.json\"\n[statistics]\nwindow_ms = 4294967296\n",
                 "statistics.window_ms");

    const auto malformedPath = WriteFixture("scenario-malformed.toml", "[general]\ntrace_file =\n");
    try
    {
        LoadTomlConfig(malformedPath);
        NS_TEST_ASSERT_MSG_EQ(true, false, "Malformed TOML accepted");
    }
    catch (const ScenarioConfigError& error)
    {
        const std::string message = error.what();
        NS_TEST_ASSERT_MSG_NE(message.find(malformedPath),
                              std::string::npos,
                              "Parse error lacks source path: " << message);
        NS_TEST_ASSERT_MSG_NE(message.find(":2:"),
                              std::string::npos,
                              "Parse error lacks line 2: " << message);
    }
}

/**
 * @ingroup tests
 *
 * Verify legacy positional parser argument forms map to the typed schema.
 */
class ValidLegacyScenarioArgumentsTestCase : public TestCase
{
  public:
    ValidLegacyScenarioArgumentsTestCase();

  private:
    void DoRun() override;
};

ValidLegacyScenarioArgumentsTestCase::ValidLegacyScenarioArgumentsTestCase()
    : TestCase("parse valid legacy scenario arguments")
{
}

void
ValidLegacyScenarioArgumentsTestCase::DoRun()
{
    const auto minimal = ParseScenarioArguments({"trace.json"});
    NS_TEST_ASSERT_MSG_EQ(minimal.valid, true, "Minimal arguments rejected");
    NS_TEST_ASSERT_MSG_EQ(minimal.config.general.traceFile, "trace.json", "Wrong trace path");
    NS_TEST_ASSERT_MSG_EQ(minimal.config.wifi.bandwidthMhz, 20, "Wrong default bandwidth");
    NS_TEST_ASSERT_MSG_EQ(minimal.config.general.outputName,
                          "mac-node-stats.json",
                          "Wrong default output name");
    NS_TEST_ASSERT_MSG_EQ(minimal.config.simulation.durationMode,
                          DurationMode::AUTO,
                          "Wrong default duration mode");

    const auto fixed = ParseScenarioArguments({"trace.json", "80", "stats.json", "2.5"});
    NS_TEST_ASSERT_MSG_EQ(fixed.valid, true, "Fixed arguments rejected");
    NS_TEST_ASSERT_MSG_EQ(fixed.config.wifi.bandwidthMhz, 80, "Wrong configured bandwidth");
    NS_TEST_ASSERT_MSG_EQ(fixed.config.general.outputName, "stats.json", "Wrong output name");
    NS_TEST_ASSERT_MSG_EQ(fixed.config.simulation.durationMode,
                          DurationMode::FIXED,
                          "Wrong fixed duration mode");
    NS_TEST_ASSERT_MSG_EQ_TOL(fixed.config.simulation.fixedDurationSeconds,
                              2.5,
                              1e-9,
                              "Wrong fixed duration");

    const auto automatic = ParseScenarioArguments({"trace.json", "40", "stats.json", "auto"});
    NS_TEST_ASSERT_MSG_EQ(automatic.valid, true, "Automatic arguments rejected");
    NS_TEST_ASSERT_MSG_EQ(automatic.config.wifi.bandwidthMhz, 40, "Wrong automatic bandwidth");
    NS_TEST_ASSERT_MSG_EQ(automatic.config.simulation.durationMode,
                          DurationMode::AUTO,
                          "Wrong automatic duration mode");
}

/**
 * @ingroup tests
 *
 * Verify legacy positional parser validation and diagnostics.
 */
class InvalidLegacyScenarioArgumentsTestCase : public TestCase
{
  public:
    InvalidLegacyScenarioArgumentsTestCase();

  private:
    void DoRun() override;
    void CheckInvalidDuration(const std::string& value);
};

InvalidLegacyScenarioArgumentsTestCase::InvalidLegacyScenarioArgumentsTestCase()
    : TestCase("reject invalid legacy scenario arguments")
{
}

void
InvalidLegacyScenarioArgumentsTestCase::CheckInvalidDuration(const std::string& value)
{
    const auto result = ParseScenarioArguments({"trace.json", "20", "stats.json", value});
    NS_TEST_ASSERT_MSG_EQ(result.valid, false, "Invalid duration accepted: " << value);
    NS_TEST_ASSERT_MSG_EQ(result.error,
                          "Invalid experiment_time: " + value +
                              ". Expected 'auto' or a positive number of seconds.",
                          "Wrong duration error");
}

void
InvalidLegacyScenarioArgumentsTestCase::DoRun()
{
    const auto missing = ParseScenarioArguments({});
    NS_TEST_ASSERT_MSG_EQ(missing.valid, false, "Missing trace accepted");
    NS_TEST_ASSERT_MSG_EQ(missing.printUsage, true, "Usage not requested");

    const auto invalidBandwidth = ParseScenarioArguments({"trace.json", "30"});
    NS_TEST_ASSERT_MSG_EQ(invalidBandwidth.valid, false, "Invalid bandwidth accepted");
    NS_TEST_ASSERT_MSG_EQ(invalidBandwidth.error,
                          "Unsupported bandwidth: 30 MHz. Expected 20, 40, 80 or 160.",
                          "Wrong bandwidth error");

    const auto tooMany =
        ParseScenarioArguments({"trace.json", "20", "stats.json", "auto", "extra"});
    NS_TEST_ASSERT_MSG_EQ(tooMany.valid, false, "Extra argument accepted");
    NS_TEST_ASSERT_MSG_EQ(tooMany.error,
                          "Too many command-line arguments.",
                          "Wrong extra argument error");

    CheckInvalidDuration("0");
    CheckInvalidDuration("-1");
    CheckInvalidDuration("invalid");
    CheckInvalidDuration("2.5seconds");
}

} // namespace

std::vector<TestCase*>
CreateScenarioConfigTestCases()
{
    return {new ScenarioConfigDefaultsTestCase,
            new ScenarioConfigRegistryTestCase,
            new ScenarioConfigTomlTestCase,
            new ValidLegacyScenarioArgumentsTestCase,
            new InvalidLegacyScenarioArgumentsTestCase};
}
