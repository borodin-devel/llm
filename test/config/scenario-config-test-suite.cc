#include "../../examples/config/scenario-config.h"
#include "../llm-test-suite.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace ns3;

namespace
{

std::string_view
ExpectedHelpType(ConfigValueType valueType)
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
    NS_TEST_ASSERT_MSG_EQ(config.general.runFolder.has_value(),
                          false,
                          "Run folder must be optional");
    NS_TEST_ASSERT_MSG_EQ(config.general.outputName, "output.json", "Wrong output name");
    NS_TEST_ASSERT_MSG_EQ(config.simulation.durationMode,
                          DurationMode::AUTO,
                          "Wrong duration mode");
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
 * Verify position-independent arguments and override precedence.
 */
class ScenarioCommandLineTestCase : public TestCase
{
  public:
    ScenarioCommandLineTestCase();

  private:
    void DoRun() override;
    std::filesystem::path WriteFixture(std::string_view contents);
    void CheckInvalid(const std::vector<std::string>& arguments,
                      const std::filesystem::path& workingDirectory,
                      std::string_view expectedMessage);
};

ScenarioCommandLineTestCase::ScenarioCommandLineTestCase()
    : TestCase("parse position-independent scenario arguments")
{
}

std::filesystem::path
ScenarioCommandLineTestCase::WriteFixture(std::string_view contents)
{
    const std::filesystem::path path = CreateTempDirFilename("scenario-cli.toml");
    std::ofstream output(path);
    output << contents;
    output.close();
    return path;
}

void
ScenarioCommandLineTestCase::CheckInvalid(const std::vector<std::string>& arguments,
                                          const std::filesystem::path& workingDirectory,
                                          std::string_view expectedMessage)
{
    const auto result = ParseScenarioArguments(arguments, workingDirectory);
    NS_TEST_ASSERT_MSG_EQ(result.valid, false, "Invalid arguments accepted");
    NS_TEST_ASSERT_MSG_NE(result.error.find(expectedMessage),
                          std::string::npos,
                          "Wrong error: " << result.error);
}

void
ScenarioCommandLineTestCase::DoRun()
{
    const auto configFile = WriteFixture("[general]\n"
                                         "trace_file = \"trace.json\"\n"
                                         "[wifi]\n"
                                         "bandwidth_mhz = 40\n");
    const auto workingDirectory = configFile.parent_path();
    const std::string relativeConfig = configFile.filename().string();
    const std::vector<std::vector<std::string>> orderedArguments{
        {"--config", relativeConfig, "--wifi-bandwidth-mhz", "80"},
        {"--wifi-bandwidth-mhz", "80", "--config", relativeConfig},
        {"--wifi-bandwidth-mhz", "80", "--simulation-rng-run", "9", "--config", relativeConfig}};
    for (const auto& arguments : orderedArguments)
    {
        const auto result = ParseScenarioArguments(arguments, workingDirectory);
        NS_TEST_ASSERT_MSG_EQ(result.valid, true, "Valid arguments rejected: " << result.error);
        NS_TEST_ASSERT_MSG_EQ(result.printUsage, false, "Valid launch requested usage");
        NS_TEST_ASSERT_MSG_EQ(result.launch.scenario.wifi.bandwidthMhz,
                              80,
                              "CLI did not override TOML");
        NS_TEST_ASSERT_MSG_EQ(result.launch.configFile, configFile, "Config path not resolved");
        NS_TEST_ASSERT_MSG_EQ(result.launch.workingDirectory,
                              workingDirectory,
                              "Working directory not retained");
    }

    const auto multiple = ParseScenarioArguments(orderedArguments.back(), workingDirectory);
    NS_TEST_ASSERT_MSG_EQ(multiple.launch.scenario.simulation.rngRun,
                          9,
                          "Second override not applied");

    const auto typed = ParseScenarioArguments({"--wifi-active-probing",
                                               "false",
                                               "--wifi-band",
                                               "6GHz",
                                               "--simulation-fixed-duration-seconds",
                                               "1.25",
                                               "--statistics-window-ms",
                                               "25",
                                               "--config",
                                               relativeConfig},
                                              workingDirectory);
    NS_TEST_ASSERT_MSG_EQ(typed.valid, true, "Typed overrides rejected: " << typed.error);
    NS_TEST_ASSERT_MSG_EQ(typed.launch.scenario.wifi.activeProbing,
                          false,
                          "Lowercase Boolean not applied");
    NS_TEST_ASSERT_MSG_EQ(typed.launch.scenario.wifi.band,
                          WifiBandConfig::BAND_6_GHZ,
                          "Strict enum not applied");
    NS_TEST_ASSERT_MSG_EQ_TOL(typed.launch.scenario.simulation.fixedDurationSeconds,
                              1.25,
                              1e-12,
                              "Floating-point override not applied");
    NS_TEST_ASSERT_MSG_EQ(typed.launch.scenario.statistics.windowMs,
                          25,
                          "Integer override not applied");

    const auto absoluteConfigFile = std::filesystem::absolute(configFile);
    const auto absolute = ParseScenarioArguments({"--config", absoluteConfigFile.string()},
                                                 workingDirectory / "unused");
    NS_TEST_ASSERT_MSG_EQ(absolute.valid, true, "Absolute config path rejected");
    NS_TEST_ASSERT_MSG_EQ(absolute.launch.configFile,
                          absoluteConfigFile,
                          "Absolute config path was changed");

    const auto negative =
        ParseScenarioArguments({"--config", relativeConfig, "--topology-bss-count", "-1"},
                               workingDirectory);
    NS_TEST_ASSERT_MSG_EQ(negative.valid, true, "Negative value parsed as a flag");
    NS_TEST_ASSERT_MSG_EQ(negative.launch.scenario.topology.bssCount,
                          -1,
                          "Negative signed integer not consumed");

    const auto help = ParseScenarioArguments({"--help"}, workingDirectory);
    NS_TEST_ASSERT_MSG_EQ(help.valid, true, "Help rejected without config");
    NS_TEST_ASSERT_MSG_EQ(help.printUsage, true, "Help did not request usage");

    std::ostringstream usage;
    PrintScenarioUsage(usage, "PROGRAM");
    const std::string usageText = usage.str();
    NS_TEST_ASSERT_MSG_NE(
        usageText.find("Usage: PROGRAM --config <config.toml> [--section-field <value> ...]"),
        std::string::npos,
        "Usage synopsis missing");
    NS_TEST_ASSERT_MSG_NE(usageText.find("--help"), std::string::npos, "Help option missing");
    const auto configSynopsis = usageText.find("--config");
    const auto configOption = usageText.find("--config", configSynopsis + 1);
    NS_TEST_ASSERT_MSG_NE(configOption, std::string::npos, "Config option missing");
    NS_TEST_ASSERT_MSG_EQ(usageText.find("--config", configOption + 1),
                          std::string::npos,
                          "Config option duplicated");
    const auto helpOption = usageText.find("--help");
    NS_TEST_ASSERT_MSG_EQ(usageText.find("--help", helpOption + 1),
                          std::string::npos,
                          "Help option duplicated");
    for (const auto& option : GetScenarioConfigOptionInfo())
    {
        const std::string optionLinePrefix = "  " + std::string(option.cliFlag) + " <" +
                                             std::string(ExpectedHelpType(option.valueType)) + ">";
        const auto flagPosition = usageText.find(optionLinePrefix);
        NS_TEST_ASSERT_MSG_NE(flagPosition, std::string::npos, "Usage missing " << option.cliFlag);
        NS_TEST_ASSERT_MSG_EQ(
            usageText.find(optionLinePrefix, flagPosition + optionLinePrefix.size()),
            std::string::npos,
            "Usage duplicates " << option.cliFlag);
        NS_TEST_ASSERT_MSG_EQ(usageText.substr(flagPosition + optionLinePrefix.size(), 2),
                              "  ",
                              "Usage lacks spacing after " << option.cliFlag);
        NS_TEST_ASSERT_MSG_NE(usageText.find(option.tomlPath, flagPosition),
                              std::string::npos,
                              "Usage lacks TOML key for " << option.cliFlag);
        NS_TEST_ASSERT_MSG_NE(usageText.find(option.description, flagPosition),
                              std::string::npos,
                              "Usage lacks description for " << option.cliFlag);
    }

    CheckInvalid({}, workingDirectory, "--config");
    CheckInvalid({"trace.json"}, workingDirectory, "positional");
    CheckInvalid({"--config"}, workingDirectory, "requires a value");
    CheckInvalid({"--config", relativeConfig, "--config", relativeConfig},
                 workingDirectory,
                 "duplicate --config");
    CheckInvalid({"--config", relativeConfig, "--wifi-bandwidth-mhz"},
                 workingDirectory,
                 "requires a value");
    CheckInvalid(
        {"--config", relativeConfig, "--wifi-bandwidth-mhz", "80", "--wifi-bandwidth-mhz", "40"},
        workingDirectory,
        "duplicate --wifi-bandwidth-mhz");
    CheckInvalid({"--config", relativeConfig, "--unknown", "value"},
                 workingDirectory,
                 "unknown flag");
    CheckInvalid({"--wifi-bandwidth-mhz", "80"}, workingDirectory, "--config");
    CheckInvalid({"--config", "missing.toml"}, workingDirectory, "regular file");
    CheckInvalid({"--config", "."}, workingDirectory, "regular file");
    CheckInvalid({"--config", relativeConfig, "--topology-isolate-bss-channels", "TRUE"},
                 workingDirectory,
                 "topology.isolate_bss_channels");
    CheckInvalid({"--config", relativeConfig, "--simulation-rng-run", "-1"},
                 workingDirectory,
                 "simulation.rng_run");
    CheckInvalid({"--config", relativeConfig, "--simulation-rng-run", "9tail"},
                 workingDirectory,
                 "simulation.rng_run");
    CheckInvalid({"--config", relativeConfig, "--statistics-window-ms", "4294967296"},
                 workingDirectory,
                 "statistics.window_ms");
    CheckInvalid({"--config", relativeConfig, "--simulation-fixed-duration-seconds", "1.5seconds"},
                 workingDirectory,
                 "simulation.fixed_duration_seconds");
    CheckInvalid({"--config", relativeConfig, "--wifi-band", "5ghz"},
                 workingDirectory,
                 "wifi.band");
    CheckInvalid({"--help", "--help"}, workingDirectory, "duplicate --help");
}

} // namespace

std::vector<TestCase*>
CreateScenarioConfigTestCases()
{
    return {new ScenarioConfigDefaultsTestCase,
            new ScenarioConfigRegistryTestCase,
            new ScenarioConfigTomlTestCase,
            new ScenarioCommandLineTestCase};
}
