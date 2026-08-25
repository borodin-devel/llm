#include "../examples/scenario-config.h"
#include "llm-test-suite.h"

#include <string>
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
            new ValidLegacyScenarioArgumentsTestCase,
            new InvalidLegacyScenarioArgumentsTestCase};
}
