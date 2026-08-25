#include "../examples/traffic-coordinator.h"
#include "../examples/wifi-statistics-internal.h"
#include "llm-test-suite.h"

#include <vector>

using namespace ns3;

namespace
{

/**
 * @ingroup tests
 *
 * Verify node-second storage distinguishes the first index beyond 32 bits.
 */
class NodeSecondIndexWidthTestCase : public TestCase
{
  public:
    NodeSecondIndexWidthTestCase();

  private:
    void DoRun() override;
};

NodeSecondIndexWidthTestCase::NodeSecondIndexWidthTestCase()
    : TestCase("preserve node-second indexes beyond 32 bits")
{
}

void
NodeSecondIndexWidthTestCase::DoRun()
{
    constexpr uint64_t firstIndexBeyond32Bits = uint64_t{1} << 32;
    constexpr int64_t firstIndexBeyond32BitsUs =
        static_cast<int64_t>(firstIndexBeyond32Bits) * 1000000;
    uint64_t resolvedIndex = 0;

    NS_TEST_ASSERT_MSG_EQ(GetNodeSecondIndex(firstIndexBeyond32BitsUs,
                                             firstIndexBeyond32BitsUs + 1000000,
                                             resolvedIndex),
                          true,
                          "Long node-second timestamp was rejected");
    NS_TEST_ASSERT_MSG_EQ(resolvedIndex,
                          firstIndexBeyond32Bits,
                          "Collected node-second index wrapped at 32 bits");

    TrafficCoordinator coordinator(1.0, 1.0);
    WifiStatisticsState state(coordinator, 10);

    state.nodeSeconds[7][0].appTxBytes = 1;
    state.nodeSeconds[7][firstIndexBeyond32Bits].appTxBytes = 2;

    NS_TEST_ASSERT_MSG_EQ(state.nodeSeconds.at(7).size(),
                          2,
                          "Node-second index wrapped at 32 bits");
    NS_TEST_ASSERT_MSG_EQ(state.nodeSeconds.at(7).at(0).appTxBytes,
                          1,
                          "First node-second bucket was overwritten");
    NS_TEST_ASSERT_MSG_EQ(state.nodeSeconds.at(7).at(firstIndexBeyond32Bits).appTxBytes,
                          2,
                          "Long node-second bucket was not preserved");
}

/**
 * @ingroup tests
 *
 * Verify that cross-layer summaries retain every registered node and interval,
 * including empty nodes and a partial final interval.
 */
class CrossLayerSummaryTestCase : public TestCase
{
  public:
    CrossLayerSummaryTestCase();

  private:
    void DoRun() override;

    /**
     * Assert that every field in an empty interval has its expected value.
     *
     * @param interval Empty interval to inspect.
     * @param index Expected interval index.
     * @param startS Expected interval start in seconds.
     * @param durationS Expected interval duration in seconds.
     */
    void AssertEmptyInterval(const CrossLayerIntervalSummary& interval,
                             uint64_t index,
                             double startS,
                             double durationS);

    /**
     * Assert every field in an empty overall record has its expected value.
     *
     * @param overall Empty overall record to inspect.
     */
    void AssertEmptyOverall(const CrossLayerOverallSummary& overall);
};

CrossLayerSummaryTestCase::CrossLayerSummaryTestCase()
    : TestCase("build complete typed cross-layer summary")
{
}

void
CrossLayerSummaryTestCase::DoRun()
{
    TrafficCoordinator coordinator(1500.0, 1500.0);
    WifiStatisticsState state(coordinator, 10);
    state.nodeLabels[7] = "AP0";
    state.nodeLabels[8] = "STA0";

    auto& first = state.nodeSeconds[7][0];
    first.appToPhy.Add(100.0);
    first.appToPhy.Add(300.0);
    first.appTxBytes = 1000000;
    first.phyPayloadBytes = 500000;
    first.phyUniquePayloadBytes = 400000;
    first.phyMpduBytes = 600000;
    first.phyBusyUs = 1250000;
    first.phyRetransmissions = 3;
    first.macTxDrops = 2;
    first.macTxDropBytes = 2000;
    first.macMpduDrops = 3;
    first.macMpduDropBytes = 1000;
    first.macMpduDropsByReason[9] = 2;
    first.macMpduDropsByReason[4] = 1;
    first.macDataFailures = 5;
    first.macFinalDataFailures = 1;
    first.appDropEvents = 5;
    first.appDropBytes = 5120;
    first.appDropsByAgent["agent-2"] = {3, 1024};
    first.appDropsByAgent["agent-1"] = {2, 4096};

    auto& last = state.nodeSeconds[7][1];
    last.appToPhy.Add(500.0);
    last.appTxBytes = 250000;
    last.phyPayloadBytes = 125000;
    last.phyUniquePayloadBytes = 100000;
    last.phyBusyUs = 125000;

    const CrossLayerSummary summary = BuildCrossLayerSummary(state);

    NS_TEST_ASSERT_MSG_EQ(summary.nodes.size(), 2, "Wrong node count");
    const auto& populated = summary.nodes.at(0);
    NS_TEST_ASSERT_MSG_EQ(populated.nodeId, 7, "Wrong populated node ID");
    NS_TEST_ASSERT_MSG_EQ(populated.nodeLabel, "AP0", "Wrong populated node label");
    NS_TEST_ASSERT_MSG_EQ(populated.oneSecondIntervals.size(), 2, "Wrong interval count");

    const auto& firstInterval = populated.oneSecondIntervals.at(0);
    NS_TEST_ASSERT_MSG_EQ(firstInterval.intervalIndex, 0, "Wrong first interval index");
    NS_TEST_ASSERT_MSG_EQ(firstInterval.intervalStartS, 0.0, "Wrong first interval start");
    NS_TEST_ASSERT_MSG_EQ(firstInterval.intervalDurationS, 1.0, "Wrong first duration");
    NS_TEST_ASSERT_MSG_EQ(firstInterval.applicationToPhyDelay.sampleCount,
                          2,
                          "Wrong first delay count");
    NS_TEST_ASSERT_MSG_EQ(firstInterval.applicationToPhyDelay.meanUs, 200.0, "Wrong delay mean");
    NS_TEST_ASSERT_MSG_EQ(firstInterval.applicationToPhyDelay.standardDeviationUs,
                          100.0,
                          "Wrong delay standard deviation");
    NS_TEST_ASSERT_MSG_EQ(firstInterval.applicationToPhyDelay.minimumUs,
                          100.0,
                          "Wrong delay minimum");
    NS_TEST_ASSERT_MSG_EQ(firstInterval.applicationToPhyDelay.maximumUs,
                          300.0,
                          "Wrong delay maximum");
    NS_TEST_ASSERT_MSG_EQ(firstInterval.applicationTransmitThroughputMbps,
                          8.0,
                          "Wrong application throughput");
    NS_TEST_ASSERT_MSG_EQ(firstInterval.phyPayloadThroughputMbps, 4.0, "Wrong PHY throughput");
    NS_TEST_ASSERT_MSG_EQ(firstInterval.uniquePhyPayloadThroughputMbps,
                          3.2,
                          "Wrong unique PHY throughput");
    NS_TEST_ASSERT_MSG_EQ(firstInterval.channelUtilizationPercent,
                          100.0,
                          "Channel utilization was not capped");
    NS_TEST_ASSERT_MSG_EQ(firstInterval.phyRetransmissionCount, 3, "Wrong retransmission count");
    NS_TEST_ASSERT_MSG_EQ(firstInterval.macTransmitDropCount, 2, "Wrong MAC transmit drops");
    NS_TEST_ASSERT_MSG_EQ(firstInterval.macTransmitDropBytes, 2000, "Wrong MAC transmit bytes");
    NS_TEST_ASSERT_MSG_EQ(firstInterval.macMpduDropCount, 3, "Wrong MAC MPDU drops");
    NS_TEST_ASSERT_MSG_EQ(firstInterval.macMpduDropBytes, 1000, "Wrong MAC MPDU bytes");
    NS_TEST_ASSERT_MSG_EQ(firstInterval.macDataFailureCount, 5, "Wrong MAC data failures");
    NS_TEST_ASSERT_MSG_EQ(firstInterval.macFinalDataFailureCount, 1, "Wrong MAC final failures");
    NS_TEST_ASSERT_MSG_EQ(firstInterval.applicationDropEventCount, 5, "Wrong application drops");
    NS_TEST_ASSERT_MSG_EQ(firstInterval.applicationDropBytes, 5120, "Wrong application drop bytes");
    NS_TEST_ASSERT_MSG_EQ(firstInterval.macMpduDropsByReason.size(), 2, "Wrong reason count");
    NS_TEST_ASSERT_MSG_EQ(firstInterval.macMpduDropsByReason.at(0).reasonCode,
                          4,
                          "Wrong reason code");
    NS_TEST_ASSERT_MSG_EQ(firstInterval.macMpduDropsByReason.at(0).dropCount,
                          1,
                          "Wrong reason drop count");
    NS_TEST_ASSERT_MSG_EQ(firstInterval.macMpduDropsByReason.at(1).reasonCode,
                          9,
                          "Wrong second reason code");
    NS_TEST_ASSERT_MSG_EQ(firstInterval.macMpduDropsByReason.at(1).dropCount,
                          2,
                          "Wrong second reason drop count");
    NS_TEST_ASSERT_MSG_EQ(firstInterval.applicationDropsByAgent.size(), 2, "Wrong agent count");
    NS_TEST_ASSERT_MSG_EQ(firstInterval.applicationDropsByAgent.at(0).agentKey,
                          "agent-1",
                          "Wrong agent key");
    NS_TEST_ASSERT_MSG_EQ(firstInterval.applicationDropsByAgent.at(0).dropEventCount,
                          2,
                          "Wrong agent drop count");
    NS_TEST_ASSERT_MSG_EQ(firstInterval.applicationDropsByAgent.at(0).droppedPayloadBytes,
                          4096,
                          "Wrong agent drop bytes");
    NS_TEST_ASSERT_MSG_EQ(firstInterval.applicationDropsByAgent.at(1).agentKey,
                          "agent-2",
                          "Wrong second agent key");
    NS_TEST_ASSERT_MSG_EQ(firstInterval.applicationDropsByAgent.at(1).dropEventCount,
                          3,
                          "Wrong second agent drop count");
    NS_TEST_ASSERT_MSG_EQ(firstInterval.applicationDropsByAgent.at(1).droppedPayloadBytes,
                          1024,
                          "Wrong second agent drop bytes");

    const auto& finalInterval = populated.oneSecondIntervals.at(1);
    NS_TEST_ASSERT_MSG_EQ(finalInterval.intervalIndex, 1, "Wrong final interval index");
    NS_TEST_ASSERT_MSG_EQ(finalInterval.intervalStartS, 1.0, "Wrong final interval start");
    NS_TEST_ASSERT_MSG_EQ(finalInterval.intervalDurationS, 0.5, "Wrong final interval duration");
    NS_TEST_ASSERT_MSG_EQ(finalInterval.applicationTransmitThroughputMbps,
                          4.0,
                          "Wrong final application throughput");
    NS_TEST_ASSERT_MSG_EQ(finalInterval.phyPayloadThroughputMbps,
                          2.0,
                          "Wrong final PHY throughput");
    NS_TEST_ASSERT_MSG_EQ(finalInterval.uniquePhyPayloadThroughputMbps,
                          1.6,
                          "Wrong final unique throughput");
    NS_TEST_ASSERT_MSG_EQ(finalInterval.channelUtilizationPercent,
                          25.0,
                          "Wrong final channel utilization");

    const auto& overall = populated.overall;
    NS_TEST_ASSERT_MSG_EQ(overall.experimentDurationS, 1.5, "Wrong experiment duration");
    NS_TEST_ASSERT_MSG_EQ(overall.applicationToPhyDelay.sampleCount, 3, "Wrong total delay count");
    NS_TEST_ASSERT_MSG_EQ(overall.applicationToPhyDelay.meanUs, 300.0, "Wrong total delay mean");
    NS_TEST_ASSERT_MSG_EQ_TOL(overall.applicationToPhyDelay.standardDeviationUs,
                              163.299316185545,
                              1e-9,
                              "Wrong total delay standard deviation");
    NS_TEST_ASSERT_MSG_EQ(overall.applicationToPhyDelay.minimumUs,
                          100.0,
                          "Wrong total delay minimum");
    NS_TEST_ASSERT_MSG_EQ(overall.applicationToPhyDelay.maximumUs,
                          500.0,
                          "Wrong total delay maximum");
    NS_TEST_ASSERT_MSG_EQ(overall.applicationTransmittedPayloadBytes,
                          1250000,
                          "Wrong total application bytes");
    NS_TEST_ASSERT_MSG_EQ(overall.phyPayloadBytes, 625000, "Wrong total PHY payload bytes");
    NS_TEST_ASSERT_MSG_EQ(overall.uniquePhyPayloadBytes, 500000, "Wrong total unique PHY bytes");
    NS_TEST_ASSERT_MSG_EQ(overall.phyMpduBytes, 600000, "Wrong total PHY MPDU bytes");
    NS_TEST_ASSERT_MSG_EQ_TOL(overall.averageApplicationTransmitThroughputMbps,
                              6.66666666666667,
                              1e-12,
                              "Wrong average application throughput");
    NS_TEST_ASSERT_MSG_EQ_TOL(overall.averagePhyPayloadThroughputMbps,
                              3.33333333333333,
                              1e-12,
                              "Wrong average PHY throughput");
    NS_TEST_ASSERT_MSG_EQ_TOL(overall.averageChannelUtilizationPercent,
                              91.6666666666667,
                              1e-12,
                              "Wrong average channel utilization");
    NS_TEST_ASSERT_MSG_EQ(overall.phyRetransmissionCount, 3, "Wrong total retransmissions");
    NS_TEST_ASSERT_MSG_EQ(overall.macTransmitDropCount, 2, "Wrong total MAC transmit drops");
    NS_TEST_ASSERT_MSG_EQ(overall.macTransmitDropBytes, 2000, "Wrong total MAC transmit bytes");
    NS_TEST_ASSERT_MSG_EQ(overall.macMpduDropCount, 3, "Wrong total MAC MPDU drops");
    NS_TEST_ASSERT_MSG_EQ(overall.macMpduDropBytes, 1000, "Wrong total MAC MPDU bytes");
    NS_TEST_ASSERT_MSG_EQ(overall.macDataFailureCount, 5, "Wrong total MAC data failures");
    NS_TEST_ASSERT_MSG_EQ(overall.macFinalDataFailureCount, 1, "Wrong total MAC final failures");
    NS_TEST_ASSERT_MSG_EQ(overall.applicationDropEventCount, 5, "Wrong total application drops");
    NS_TEST_ASSERT_MSG_EQ(overall.applicationDropBytes, 5120, "Wrong total application drop bytes");
    NS_TEST_ASSERT_MSG_EQ(overall.macMpduDropsByReason.size(), 2, "Wrong total reason count");
    NS_TEST_ASSERT_MSG_EQ(overall.macMpduDropsByReason.at(0).reasonCode,
                          4,
                          "Wrong total reason code");
    NS_TEST_ASSERT_MSG_EQ(overall.macMpduDropsByReason.at(0).dropCount,
                          1,
                          "Wrong total reason drops");
    NS_TEST_ASSERT_MSG_EQ(overall.macMpduDropsByReason.at(1).reasonCode,
                          9,
                          "Wrong second total reason code");
    NS_TEST_ASSERT_MSG_EQ(overall.macMpduDropsByReason.at(1).dropCount,
                          2,
                          "Wrong second total reason drops");

    const auto& empty = summary.nodes.at(1);
    NS_TEST_ASSERT_MSG_EQ(empty.nodeId, 8, "Wrong empty node ID");
    NS_TEST_ASSERT_MSG_EQ(empty.nodeLabel, "STA0", "Wrong empty node label");
    NS_TEST_ASSERT_MSG_EQ(empty.oneSecondIntervals.size(), 2, "Wrong empty interval count");
    AssertEmptyInterval(empty.oneSecondIntervals.at(0), 0, 0.0, 1.0);
    AssertEmptyInterval(empty.oneSecondIntervals.at(1), 1, 1.0, 0.5);
    AssertEmptyOverall(empty.overall);
}

void
CrossLayerSummaryTestCase::AssertEmptyInterval(const CrossLayerIntervalSummary& interval,
                                               uint64_t index,
                                               double startS,
                                               double durationS)
{
    NS_TEST_ASSERT_MSG_EQ(interval.intervalIndex, index, "Wrong empty interval index");
    NS_TEST_ASSERT_MSG_EQ(interval.intervalStartS, startS, "Wrong empty interval start");
    NS_TEST_ASSERT_MSG_EQ(interval.intervalDurationS, durationS, "Wrong empty interval duration");
    NS_TEST_ASSERT_MSG_EQ(interval.applicationToPhyDelay.sampleCount,
                          0,
                          "Empty interval has delay samples");
    NS_TEST_ASSERT_MSG_EQ(interval.applicationToPhyDelay.meanUs,
                          0.0,
                          "Empty interval has a delay mean");
    NS_TEST_ASSERT_MSG_EQ(interval.applicationToPhyDelay.standardDeviationUs,
                          0.0,
                          "Empty interval has a delay standard deviation");
    NS_TEST_ASSERT_MSG_EQ(interval.applicationToPhyDelay.minimumUs,
                          0.0,
                          "Empty interval has a delay minimum");
    NS_TEST_ASSERT_MSG_EQ(interval.applicationToPhyDelay.maximumUs,
                          0.0,
                          "Empty interval has a delay maximum");
    NS_TEST_ASSERT_MSG_EQ(interval.applicationTransmitThroughputMbps,
                          0.0,
                          "Empty interval has application throughput");
    NS_TEST_ASSERT_MSG_EQ(interval.phyPayloadThroughputMbps,
                          0.0,
                          "Empty interval has PHY throughput");
    NS_TEST_ASSERT_MSG_EQ(interval.uniquePhyPayloadThroughputMbps,
                          0.0,
                          "Empty interval has unique PHY throughput");
    NS_TEST_ASSERT_MSG_EQ(interval.channelUtilizationPercent,
                          0.0,
                          "Empty interval has utilization");
    NS_TEST_ASSERT_MSG_EQ(interval.phyRetransmissionCount, 0, "Empty interval has retransmissions");
    NS_TEST_ASSERT_MSG_EQ(interval.macTransmitDropCount,
                          0,
                          "Empty interval has MAC transmit drops");
    NS_TEST_ASSERT_MSG_EQ(interval.macTransmitDropBytes,
                          0,
                          "Empty interval has MAC transmit-drop bytes");
    NS_TEST_ASSERT_MSG_EQ(interval.macMpduDropCount, 0, "Empty interval has MAC MPDU drops");
    NS_TEST_ASSERT_MSG_EQ(interval.macMpduDropBytes, 0, "Empty interval has MAC MPDU-drop bytes");
    NS_TEST_ASSERT_MSG_EQ(interval.macDataFailureCount, 0, "Empty interval has MAC data failures");
    NS_TEST_ASSERT_MSG_EQ(interval.macFinalDataFailureCount,
                          0,
                          "Empty interval has final MAC data failures");
    NS_TEST_ASSERT_MSG_EQ(interval.applicationDropEventCount,
                          0,
                          "Empty interval has application drops");
    NS_TEST_ASSERT_MSG_EQ(interval.applicationDropBytes,
                          0,
                          "Empty interval has application drop bytes");
    NS_TEST_ASSERT_MSG_EQ(interval.macMpduDropsByReason.empty(),
                          true,
                          "Empty interval has drop reasons");
    NS_TEST_ASSERT_MSG_EQ(interval.applicationDropsByAgent.empty(),
                          true,
                          "Empty interval has agent drops");
}

void
CrossLayerSummaryTestCase::AssertEmptyOverall(const CrossLayerOverallSummary& overall)
{
    NS_TEST_ASSERT_MSG_EQ(overall.experimentDurationS, 1.5, "Wrong empty experiment duration");
    NS_TEST_ASSERT_MSG_EQ(overall.applicationToPhyDelay.sampleCount,
                          0,
                          "Empty overall has delay samples");
    NS_TEST_ASSERT_MSG_EQ(overall.applicationToPhyDelay.meanUs,
                          0.0,
                          "Empty overall has a delay mean");
    NS_TEST_ASSERT_MSG_EQ(overall.applicationToPhyDelay.standardDeviationUs,
                          0.0,
                          "Empty overall has a delay standard deviation");
    NS_TEST_ASSERT_MSG_EQ(overall.applicationToPhyDelay.minimumUs,
                          0.0,
                          "Empty overall has a delay minimum");
    NS_TEST_ASSERT_MSG_EQ(overall.applicationToPhyDelay.maximumUs,
                          0.0,
                          "Empty overall has a delay maximum");
    NS_TEST_ASSERT_MSG_EQ(overall.applicationTransmittedPayloadBytes,
                          0,
                          "Empty overall has application bytes");
    NS_TEST_ASSERT_MSG_EQ(overall.phyPayloadBytes, 0, "Empty overall has PHY payload bytes");
    NS_TEST_ASSERT_MSG_EQ(overall.uniquePhyPayloadBytes, 0, "Empty overall has unique PHY bytes");
    NS_TEST_ASSERT_MSG_EQ(overall.phyMpduBytes, 0, "Empty overall has PHY MPDU bytes");
    NS_TEST_ASSERT_MSG_EQ(overall.averageApplicationTransmitThroughputMbps,
                          0.0,
                          "Empty overall has application throughput");
    NS_TEST_ASSERT_MSG_EQ(overall.averagePhyPayloadThroughputMbps,
                          0.0,
                          "Empty overall has PHY throughput");
    NS_TEST_ASSERT_MSG_EQ(overall.averageChannelUtilizationPercent,
                          0.0,
                          "Empty overall has utilization");
    NS_TEST_ASSERT_MSG_EQ(overall.phyRetransmissionCount, 0, "Empty overall has retransmissions");
    NS_TEST_ASSERT_MSG_EQ(overall.macTransmitDropCount, 0, "Empty overall has MAC transmit drops");
    NS_TEST_ASSERT_MSG_EQ(overall.macTransmitDropBytes,
                          0,
                          "Empty overall has MAC transmit-drop bytes");
    NS_TEST_ASSERT_MSG_EQ(overall.macMpduDropCount, 0, "Empty overall has MAC MPDU drops");
    NS_TEST_ASSERT_MSG_EQ(overall.macMpduDropBytes, 0, "Empty overall has MAC MPDU-drop bytes");
    NS_TEST_ASSERT_MSG_EQ(overall.macDataFailureCount, 0, "Empty overall has MAC data failures");
    NS_TEST_ASSERT_MSG_EQ(overall.macFinalDataFailureCount,
                          0,
                          "Empty overall has final MAC data failures");
    NS_TEST_ASSERT_MSG_EQ(overall.applicationDropEventCount,
                          0,
                          "Empty overall has application drops");
    NS_TEST_ASSERT_MSG_EQ(overall.applicationDropBytes,
                          0,
                          "Empty overall has application drop bytes");
    NS_TEST_ASSERT_MSG_EQ(overall.macMpduDropsByReason.empty(),
                          true,
                          "Empty overall has drop reasons");
}

} // namespace

std::vector<TestCase*>
CreateCrossLayerSummaryTestCases()
{
    return {new NodeSecondIndexWidthTestCase, new CrossLayerSummaryTestCase};
}
