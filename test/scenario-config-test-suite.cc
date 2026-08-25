#include "../examples/scenario-config.h"
#include "llm-test-suite.h"

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

} // namespace

std::vector<TestCase*>
CreateScenarioConfigTestCases()
{
    return {new ScenarioConfigDefaultsTestCase};
}
