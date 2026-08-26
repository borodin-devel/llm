#include "../../llm-test-suite.h"

#include "ns3/agent-distribution.h"
#include "ns3/packet.h"

#include <vector>

using namespace ns3;

namespace
{

/**
 * @ingroup tests
 *
 * Characterize serialization of application transmit metadata.
 */
class AppTxTagTestCase : public TestCase
{
  public:
    AppTxTagTestCase();

  private:
    void DoRun() override;
};

AppTxTagTestCase::AppTxTagTestCase()
    : TestCase("round-trip application transmit byte tag")
{
}

void
AppTxTagTestCase::DoRun()
{
    AppTxTag original(42,
                      123456,
                      Ipv4Address("10.1.0.1"),
                      Ipv4Address("10.1.0.2"),
                      10000,
                      9000,
                      64,
                      "1_planner");
    Ptr<Packet> packet = Create<Packet>(64);
    packet->AddByteTag(original);

    AppTxTag restored;
    const bool found = packet->FindFirstMatchingByteTag(restored);

    NS_TEST_ASSERT_MSG_EQ(found, true, "AppTxTag was not found");
    NS_TEST_ASSERT_MSG_EQ(restored.GetAppPacketUid(), 42, "Wrong packet UID");
    NS_TEST_ASSERT_MSG_EQ(restored.GetAppTxTimeUs(), 123456, "Wrong transmit time");
    NS_TEST_ASSERT_MSG_EQ(restored.GetSource(), Ipv4Address("10.1.0.1"), "Wrong source");
    NS_TEST_ASSERT_MSG_EQ(restored.GetDestination(), Ipv4Address("10.1.0.2"), "Wrong destination");
    NS_TEST_ASSERT_MSG_EQ(restored.GetSourcePort(), 10000, "Wrong source port");
    NS_TEST_ASSERT_MSG_EQ(restored.GetDestinationPort(), 9000, "Wrong destination port");
    NS_TEST_ASSERT_MSG_EQ(restored.GetAppPayloadBytes(), 64, "Wrong payload size");
    NS_TEST_ASSERT_MSG_EQ(restored.GetAgentKey(), "1_planner", "Wrong agent key");
}

} // namespace

std::vector<TestCase*>
CreateAppTxTagTestCases()
{
    return {new AppTxTagTestCase};
}
