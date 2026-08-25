#include "../examples/traffic-flow-monitor-internal.h"
#include "llm-test-suite.h"

#include <vector>

using namespace ns3;

namespace
{

/**
 * @ingroup tests
 *
 * Verify that transmission summaries retain unmatched senders and sum only
 * strictly positive matched durations.
 */
class TransmissionSummaryTestCase : public TestCase
{
  public:
    TransmissionSummaryTestCase();

  private:
    void DoRun() override;
};

TransmissionSummaryTestCase::TransmissionSummaryTestCase()
    : TestCase("build transmission summary from positive matched pairs")
{
}

void
TransmissionSummaryTestCase::DoRun()
{
    TrafficFlowMonitorState state;
    const TrafficFlowKey matched{"10.1.0.2", 9000, "10.1.0.1", 10000, 1000};
    state.transmitTimestampsByFlow[matched] = {100, 500, 900};
    state.receiveTimestampsByFlow[matched] = {300, 800, 850};
    state.transmittedBytesBySource["10.1.0.2"] = {1000, 1000, 1000};
    state.transmittedBytesBySource["10.1.0.3"] = {3000000000ULL, 1000ULL};

    const TransmissionSummary summary = BuildTransmissionSummary(state);

    NS_TEST_ASSERT_MSG_EQ(summary.senders.size(), 2, "Wrong sender count");

    const auto& matchedSender = summary.senders.at(0);
    NS_TEST_ASSERT_MSG_EQ(matchedSender.senderIpv4, "10.1.0.2", "Wrong matched sender");
    NS_TEST_ASSERT_MSG_EQ(matchedSender.matchedPacketCount, 2, "Wrong positive match count");
    NS_TEST_ASSERT_MSG_EQ(matchedSender.totalTransmissionDurationUs, 500, "Wrong duration");
    NS_TEST_ASSERT_MSG_EQ(matchedSender.transmittedPayloadBytes, 3000, "Wrong byte count");
    NS_TEST_ASSERT_MSG_EQ(matchedSender.effectiveThroughputMbps.has_value(),
                          true,
                          "Missing effective throughput");
    NS_TEST_ASSERT_MSG_EQ(matchedSender.effectiveThroughputMbps.value(),
                          48.0,
                          "Wrong effective throughput");

    const auto& unmatchedSender = summary.senders.at(1);
    NS_TEST_ASSERT_MSG_EQ(unmatchedSender.senderIpv4, "10.1.0.3", "Wrong unmatched sender");
    NS_TEST_ASSERT_MSG_EQ(unmatchedSender.matchedPacketCount, 0, "Wrong unmatched match count");
    NS_TEST_ASSERT_MSG_EQ(unmatchedSender.totalTransmissionDurationUs,
                          0,
                          "Wrong unmatched duration");
    NS_TEST_ASSERT_MSG_EQ(unmatchedSender.transmittedPayloadBytes,
                          3000001000ULL,
                          "Wrong 64-bit byte count");
    NS_TEST_ASSERT_MSG_EQ(unmatchedSender.effectiveThroughputMbps.has_value(),
                          false,
                          "Unexpected unmatched effective throughput");
}

} // namespace

std::vector<TestCase*>
CreateTrafficFlowSummaryTestCases()
{
    return {new TransmissionSummaryTestCase};
}
