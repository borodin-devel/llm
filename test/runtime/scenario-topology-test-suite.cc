#include "../../examples/runtime/topology.h"
#include "../llm-test-suite.h"

#include <vector>

using namespace ns3;

namespace
{

/**
 * @ingroup tests
 *
 * Verify shared and isolated BSS channel selection.
 */
class BssChannelSelectionTestCase : public TestCase
{
  public:
    BssChannelSelectionTestCase();

  private:
    void DoRun() override;
};

BssChannelSelectionTestCase::BssChannelSelectionTestCase()
    : TestCase("select shared or distinct isolated BSS channels")
{
}

void
BssChannelSelectionTestCase::DoRun()
{
    Ptr<YansWifiChannel> shared = CreateDefaultYansChannel();
    NS_TEST_ASSERT_MSG_EQ(SelectBssChannel(false, shared), shared, "Shared channel changed");
    NS_TEST_ASSERT_MSG_NE(SelectBssChannel(true, shared),
                          shared,
                          "Isolation reused shared channel");
    NS_TEST_ASSERT_MSG_NE(SelectBssChannel(true, shared),
                          SelectBssChannel(true, shared),
                          "Two isolated BSSs shared one channel");
}

/**
 * @ingroup tests
 *
 * Verify Wi-Fi channel settings serialization.
 */
class ChannelSettingsTestCase : public TestCase
{
  public:
    ChannelSettingsTestCase();

  private:
    void DoRun() override;
};

ChannelSettingsTestCase::ChannelSettingsTestCase()
    : TestCase("build the configured Wi-Fi channel tuple")
{
}

void
ChannelSettingsTestCase::DoRun()
{
    WifiConfig wifi;
    wifi.channelNumber = 36;
    wifi.bandwidthMhz = 80;
    wifi.primary20Index = 2;
    NS_TEST_ASSERT_MSG_EQ(BuildChannelSettings(wifi),
                          "{36, 80, BAND_5GHZ, 2}",
                          "Wrong channel tuple");
}

} // namespace

std::vector<TestCase*>
CreateScenarioTopologyTestCases()
{
    return {new BssChannelSelectionTestCase, new ChannelSettingsTestCase};
}
