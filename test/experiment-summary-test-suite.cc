#include "../examples/experiment-window-output.h"
#include "../examples/traffic-coordinator.h"
#include "../examples/wifi-statistics-internal.h"
#include "../examples/wifi-statistics.h"
#include "llm-test-suite.h"

#include "ns3/ap-generator.h"
#include "ns3/network-module.h"

#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>

using namespace ns3;

namespace
{

/** Open a deterministic experiment epoch for summary tests. */
class SummaryFixture
{
  public:
    /**
     * Construct and open an experiment.
     *
     * @param durationMs Exact experiment duration in milliseconds.
     * @param windowMs Configured statistics window in milliseconds.
     */
    SummaryFixture(double durationMs, uint32_t windowMs)
        : coordinator(durationMs, durationMs),
          statistics(coordinator, windowMs)
    {
        Ptr<Node> node = CreateObject<Node>();
        Ptr<APGenerator> generator = CreateObject<APGenerator>();
        generator->SetReadyCallback(coordinator.GetReadyCallback());
        node->AddApplication(generator);
        generator->SetStartTime(Seconds(0));
        coordinator.AddGenerator(generator);
        coordinator.AddApplication(generator);
        coordinator.FinalizeRegistration();
        Simulator::Stop(MicroSeconds(1));
        Simulator::Run();
    }

    /** Close simulator state. */
    ~SummaryFixture()
    {
        Simulator::Destroy();
    }

    TrafficCoordinator coordinator; ///< Experiment timing owner.
    WifiStatistics statistics;      ///< Statistics owner under test.
};

void
SetAccepted(AppDirectionAccumulator& app,
            uint32_t peerNodeId,
            const std::string& agentKey,
            uint64_t bytes)
{
    app.acceptedSendCount = 1;
    app.acceptedPayloadBytes = bytes;
    app.agents[agentKey].acceptedSendCount = 1;
    app.agents[agentKey].acceptedPayloadBytes = bytes;
    app.peersByNodeId[peerNodeId].acceptedSendCount = 1;
    app.peersByNodeId[peerNodeId].acceptedPayloadBytes = bytes;
}

void
SetPhy(PhyDirectionAccumulator& phy,
       uint32_t peerNodeId,
       uint64_t bytes,
       uint64_t attempts,
       long double rateBpsUs,
       long double airtimeUs)
{
    phy.taggedPayloadBytes = bytes;
    phy.uniqueTaggedPayloadBytes = bytes;
    phy.transmissionAttemptCount = attempts;
    phy.dataRateBpsUs = rateBpsUs;
    phy.transmissionAirtimeUs = airtimeUs;
    auto& peer = phy.peersByNodeId[peerNodeId];
    peer.taggedPayloadBytes = bytes;
    peer.uniqueTaggedPayloadBytes = bytes;
    peer.transmissionAttemptCount = attempts;
    peer.dataRateBpsUs = rateBpsUs;
    peer.transmissionAirtimeUs = airtimeUs;
}

} // namespace

/** @ingroup tests Verify sparse windows and exact AP BSS-parent attribution. */
class ExperimentSummaryTestCase : public TestCase
{
  public:
    ExperimentSummaryTestCase();

  private:
    void DoRun() override;
};

ExperimentSummaryTestCase::ExperimentSummaryTestCase()
    : TestCase("build sparse fixed-shape BSS-parent windows")
{
}

void
ExperimentSummaryTestCase::DoRun()
{
    SummaryFixture fixture(30.0, 10);
    auto& statistics = fixture.statistics;
    statistics.RegisterAccessPointIdentity(0, 10, "AP0", Ipv4Address("10.1.0.1"));
    statistics.RegisterStationIdentity(0, 0, 20, "STA0", Ipv4Address("10.1.0.2"));
    statistics.RegisterStationIdentity(0, 1, 21, "STA1", Ipv4Address("10.1.0.3"));

    auto& window0 = statistics.m_state->unifiedWindows[0];
    SetAccepted(window0[20].app.uplink, 10, "agent-z", 1000);
    SetAccepted(window0[21].app.uplink, 10, "agent-a", 2000);
    auto& firstTcp = window0[20].tcpConnections[{ExperimentDirection::UPLINK, 10}];
    firstTcp.congestionWindowObservationDurationUs = 10000;
    firstTcp.congestionWindowBytesUs = 12000000.0L;
    firstTcp.lastCongestionWindowBytes = 1400;
    auto& secondTcp = window0[21].tcpConnections[{ExperimentDirection::UPLINK, 10}];
    secondTcp.congestionWindowObservationDurationUs = 10000;
    secondTcp.congestionWindowBytesUs = 22000000.0L;
    secondTcp.lastCongestionWindowBytes = 2400;
    window0[10].phy.busyTimeUs = 1000;
    window0[20].phy.busyTimeUs = 6000;
    window0[20].mac.uplink.mpduDropCount = 3;
    window0[20].mac.uplink.mpduDropBytes = 350;
    window0[20].mac.uplink.dataFailureCount = 5;
    window0[20].mac.uplink.finalDataFailureCount = 3;
    window0[20].mac.uplink.mpduDropsByReason[9] = 1;
    window0[20].mac.uplink.mpduDropsByReason[7] = 1;
    window0[20].mac.uplink.mpduDropsByReason[4] = 1;
    auto& stationMacPeer = window0[20].mac.uplink.peersByNodeId[10];
    stationMacPeer.mpduDropCount = 2;
    stationMacPeer.mpduDropBytes = 300;
    stationMacPeer.dataFailureCount = 3;
    stationMacPeer.finalDataFailureCount = 1;
    stationMacPeer.mpduDropsByReason[9] = 1;
    stationMacPeer.mpduDropsByReason[4] = 1;

    auto& window1 = statistics.m_state->unifiedWindows[1];
    SetAccepted(window1[10].app.downlink, 20, "agent-d", 4000);
    window1[10].app.uplink.receiveEventCount = 1;
    window1[10].app.uplink.receivedPayloadBytes = 2500;
    window1[10].app.uplink.peersByNodeId[20].receiveEventCount = 1;
    window1[10].app.uplink.peersByNodeId[20].receivedPayloadBytes = 2500;

    statistics.m_state->unifiedWindows[2][10];
    statistics.m_state->unifiedWindows[2][20];

    const UnifiedExperimentSummary summary = statistics.BuildUnifiedExperimentSummary();
    NS_TEST_ASSERT_MSG_EQ(summary.windows.size(), 2, "Empty final window was emitted");
    NS_TEST_ASSERT_MSG_EQ(summary.windows.at(0).windowIndex, 0, "Wrong first window index");
    NS_TEST_ASSERT_MSG_EQ(summary.windows.at(0).accessPoints.size(), 1, "Missing AP parent");
    NS_TEST_ASSERT_MSG_EQ(summary.windows.at(0).stations.size(), 2, "Missing active stations");

    const auto& parent0 = summary.windows.at(0).accessPoints.at(0).statistics;
    NS_TEST_ASSERT_MSG_EQ(parent0.appStats.uplink.acceptedPayloadBytes,
                          3000,
                          "AP did not merge child uplink accepted bytes");
    NS_TEST_ASSERT_MSG_EQ(parent0.appStats.uplink.agents.at(0).agentKey,
                          "agent-a",
                          "Agents are not deterministic");
    NS_TEST_ASSERT_MSG_EQ(parent0.appStats.uplink.peers.at(0).peerNodeId,
                          20,
                          "AP peer detail did not use station identity");
    NS_TEST_ASSERT_MSG_EQ(parent0.tcpStats.uplink.connections.size(),
                          2,
                          "AP combined independent TCP peers");
    NS_TEST_ASSERT_MSG_EQ(parent0.tcpStats.uplink.connections.at(0).peerNodeId,
                          20,
                          "Wrong first AP TCP peer");
    NS_TEST_ASSERT_MSG_EQ(parent0.tcpStats.uplink.connections.at(1).peerNodeId,
                          21,
                          "Wrong second AP TCP peer");
    NS_TEST_ASSERT_MSG_EQ(parent0.phyStats.busyTimeUs, 1000, "AP busy time included child PHY");
    NS_TEST_ASSERT_MSG_EQ(parent0.macStats.uplink.peers.at(0).mpduDropBytes,
                          300,
                          "AP peer lost child MPDU drop bytes");
    NS_TEST_ASSERT_MSG_EQ(parent0.macStats.uplink.peers.at(0).dataFailureCount,
                          3,
                          "AP peer lost child data failures");
    NS_TEST_ASSERT_MSG_EQ(parent0.macStats.uplink.peers.at(0).finalDataFailureCount,
                          1,
                          "AP peer lost child final failures");
    NS_TEST_ASSERT_MSG_EQ(parent0.macStats.uplink.peers.at(0).mpduDropsByReason.at(0).reasonCode,
                          4,
                          "AP peer reasons are not numerically ordered");
    NS_TEST_ASSERT_MSG_EQ(parent0.macStats.uplink.mpduDropsByReason.at(0).reasonCode,
                          4,
                          "AP direction reasons lost numeric order");
    NS_TEST_ASSERT_MSG_EQ(parent0.macStats.uplink.mpduDropsByReason.at(1).reasonCode,
                          7,
                          "Unresolved AP direction reason was not retained");
    NS_TEST_ASSERT_MSG_EQ(parent0.macStats.uplink.mpduDropsByReason.at(2).reasonCode,
                          9,
                          "AP direction reasons lost final numeric value");
    NS_TEST_ASSERT_MSG_EQ(parent0.macStats.uplink.peers.at(0).mpduDropCount,
                          2,
                          "Unresolved MPDU drop leaked into AP peer detail");
    NS_TEST_ASSERT_MSG_EQ(parent0.macStats.uplink.mpduDropCount,
                          3,
                          "Unresolved MPDU drop was lost from AP direction totals");
    NS_TEST_ASSERT_MSG_EQ(parent0.macStats.uplink.mpduDropBytes,
                          350,
                          "Unresolved MPDU bytes were lost from AP direction totals");
    NS_TEST_ASSERT_MSG_EQ(parent0.macStats.uplink.dataFailureCount,
                          5,
                          "Unresolved data failures were lost from AP direction totals");
    NS_TEST_ASSERT_MSG_EQ(parent0.macStats.uplink.finalDataFailureCount,
                          3,
                          "Unresolved final failures were lost from AP direction totals");
    NS_TEST_ASSERT_MSG_EQ(
        summary.windows.at(0).stations.at(0).statistics.appStats.uplink.acceptedPayloadBytes,
        1000,
        "Wrong STA0 child value");
    NS_TEST_ASSERT_MSG_EQ(summary.windows.at(0).stations.at(0).statistics.phyStats.busyTimeUs,
                          6000,
                          "Station busy time did not remain local");
    NS_TEST_ASSERT_MSG_EQ(
        summary.windows.at(0).stations.at(1).statistics.appStats.uplink.acceptedPayloadBytes,
        2000,
        "Wrong STA1 child value");

    const auto& parent1 = summary.windows.at(1).accessPoints.at(0).statistics;
    NS_TEST_ASSERT_MSG_EQ(parent1.appStats.downlink.acceptedPayloadBytes,
                          4000,
                          "Wrong AP downlink sender value");
    NS_TEST_ASSERT_MSG_EQ(parent1.appStats.downlink.peers.at(0).peerNodeId,
                          20,
                          "Missing AP destination detail");
    NS_TEST_ASSERT_MSG_EQ(parent1.appStats.uplink.receivedPayloadBytes,
                          2500,
                          "AP receiver value was mixed with sender value");
    NS_TEST_ASSERT_MSG_EQ(parent1.appStats.uplink.acceptedPayloadBytes,
                          0,
                          "AP receiver bytes became accepted bytes");
    NS_TEST_ASSERT_MSG_EQ(summary.windows.at(1).stations.empty(),
                          true,
                          "Inactive station was emitted");

    NS_TEST_ASSERT_MSG_EQ(parent0.generalStats.downlink.matchedPacketCount,
                          0,
                          "Missing direction object has nonzero data");
    NS_TEST_ASSERT_MSG_EQ(parent0.macStats.downlink.mpduDropsByReason.empty(),
                          true,
                          "Missing MAC category shape is not empty");
    NS_TEST_ASSERT_MSG_EQ(parent0.phyStats.downlink.peers.empty(),
                          true,
                          "Missing PHY direction shape is not empty");
}

/** @ingroup tests Verify raw overall formulas, partial duration, and null values. */
class ExperimentOverallTestCase : public TestCase
{
  public:
    ExperimentOverallTestCase();

  private:
    void DoRun() override;
};

ExperimentOverallTestCase::ExperimentOverallTestCase()
    : TestCase("merge raw overall statistics with actual denominators")
{
}

void
ExperimentOverallTestCase::DoRun()
{
    SummaryFixture fixture(35.0, 25);
    auto& statistics = fixture.statistics;
    statistics.RegisterAccessPointIdentity(0, 10, "AP0", Ipv4Address("10.1.0.1"));
    statistics.RegisterStationIdentity(0, 0, 20, "STA0", Ipv4Address("10.1.0.2"));
    statistics.RegisterStationIdentity(0, 1, 21, "STA1", Ipv4Address("10.1.0.3"));

    auto& firstSta = statistics.m_state->unifiedWindows[0][20];
    auto& secondSta = statistics.m_state->unifiedWindows[1][20];
    SetAccepted(firstSta.app.uplink, 10, "agent-z", 1000);
    SetAccepted(secondSta.app.uplink, 10, "agent-a", 1000);
    firstSta.applicationToPhyDelayUs.uplink.Add(10.0);
    firstSta.applicationToPhyDelayUs.uplink.Add(30.0);
    secondSta.applicationToPhyDelayUs.uplink.Add(50.0);
    SetPhy(firstSta.phy.uplink, 10, 1000, 1, 10e6L * 100.0L, 100.0L);
    SetPhy(secondSta.phy.uplink, 10, 1000, 1, 20e6L * 300.0L, 300.0L);
    SetPhy(statistics.m_state->unifiedWindows[0][10].phy.uplink,
           20,
           1000,
           1,
           10e6L * 100.0L,
           100.0L);
    SetPhy(statistics.m_state->unifiedWindows[1][10].phy.uplink,
           20,
           1000,
           1,
           20e6L * 300.0L,
           300.0L);
    statistics.m_state->unifiedWindows[0][10].phy.busyTimeUs = 5000;
    statistics.m_state->unifiedWindows[1][10].phy.busyTimeUs = 2000;

    auto& firstTcp = firstSta.tcpConnections[{ExperimentDirection::UPLINK, 10}];
    firstTcp.congestionWindowObservationDurationUs = 25000;
    firstTcp.congestionWindowBytesUs = 25000000.0L;
    firstTcp.lastCongestionWindowBytes = 1000;
    auto& secondTcp = secondSta.tcpConnections[{ExperimentDirection::UPLINK, 10}];
    secondTcp.congestionWindowObservationDurationUs = 10000;
    secondTcp.congestionWindowBytesUs = 30000000.0L;
    secondTcp.lastCongestionWindowBytes = 3000;
    statistics.m_state->unifiedWindows[0][21].tcpConnections[{ExperimentDirection::UPLINK, 10}];

    const UnifiedExperimentSummary summary = statistics.BuildUnifiedExperimentSummary();
    NS_TEST_ASSERT_MSG_EQ(summary.windows.at(1).windowDurationMs,
                          10.0,
                          "Wrong partial-window duration");
    NS_TEST_ASSERT_MSG_EQ(summary.overall.stations.size(), 2, "Overall is not inventory-dense");
    const auto& station = summary.overall.stations.at(0).statistics;
    NS_TEST_ASSERT_MSG_EQ_TOL(station.appStats.uplink.acceptedThroughputMbps.value(),
                              16000.0 / 35000.0,
                              1e-12,
                              "Overall throughput used active windows");
    NS_TEST_ASSERT_MSG_EQ_TOL(station.generalStats.uplink.applicationToPhyDelay.averageUs.value(),
                              30.0,
                              1e-12,
                              "Raw delay samples were not merged");
    NS_TEST_ASSERT_MSG_EQ(station.generalStats.uplink.applicationToPhyDelay.sampleCount,
                          3,
                          "Wrong merged delay sample count");
    NS_TEST_ASSERT_MSG_EQ(station.generalStats.uplink.applicationToPhyDelay.minimumUs.value(),
                          10.0,
                          "Wrong merged delay minimum");
    NS_TEST_ASSERT_MSG_EQ(station.generalStats.uplink.applicationToPhyDelay.maximumUs.value(),
                          50.0,
                          "Wrong merged delay maximum");
    NS_TEST_ASSERT_MSG_EQ_TOL(
        station.generalStats.uplink.applicationToPhyDelay.standardDeviationUs.value(),
        std::sqrt(800.0 / 3.0),
        1e-12,
        "Wrong merged population deviation");
    NS_TEST_ASSERT_MSG_EQ_TOL(station.phyStats.uplink.averageDataRateMbps.value(),
                              17.5,
                              1e-12,
                              "PHY rates were averaged instead of weighted");
    NS_TEST_ASSERT_MSG_EQ_TOL(
        summary.overall.accessPoints.at(0).statistics.phyStats.channelUtilizationPercent.value(),
        20.0,
        1e-12,
        "Overall AP utilization used the wrong busy source");
    NS_TEST_ASSERT_MSG_EQ_TOL(
        station.tcpStats.uplink.connections.at(0).averageCongestionWindowBytes.value(),
        55000000.0 / 35000.0,
        1e-12,
        "CWND was not weighted by observed duration");
    NS_TEST_ASSERT_MSG_EQ(station.appStats.uplink.agents.at(0).agentKey,
                          "agent-a",
                          "Overall agents are not deterministic");
    NS_TEST_ASSERT_MSG_EQ_TOL(
        station.appStats.uplink.agents.at(0).acceptedBandwidthSharePercent.value(),
        50.0,
        1e-12,
        "Overall agent share used a window denominator");
    NS_TEST_ASSERT_MSG_EQ_TOL(
        station.appStats.uplink.peers.at(0).acceptedBandwidthSharePercent.value(),
        100.0,
        1e-12,
        "Overall peer share used a window denominator");

    const auto& zero = summary.overall.stations.at(1).statistics;
    NS_TEST_ASSERT_MSG_EQ(zero.appStats.uplink.acceptedThroughputMbps.value(),
                          0.0,
                          "Known zero throughput is not numeric zero");
    NS_TEST_ASSERT_MSG_EQ(zero.phyStats.uplink.averageDataRateMbps.has_value(),
                          false,
                          "Zero-airtime PHY average is not null");
    NS_TEST_ASSERT_MSG_EQ(zero.generalStats.uplink.applicationToPhyDelay.averageUs.has_value(),
                          false,
                          "Zero-sample delay average is not null");
    NS_TEST_ASSERT_MSG_EQ(
        zero.tcpStats.uplink.connections.at(0).averageCongestionWindowBytes.has_value(),
        false,
        "Zero-observation CWND average is not null");
    NS_TEST_ASSERT_MSG_EQ(
        zero.tcpStats.uplink.connections.at(0).lastCongestionWindowBytes.has_value(),
        false,
        "Missing terminal CWND is not null");

    const EntityStatisticsOutput noDuration =
        FinalizeEntityStatistics(LocalEntityWindowAccumulator{}, 0, *statistics.m_state);
    NS_TEST_ASSERT_MSG_EQ(noDuration.appStats.uplink.acceptedThroughputMbps.has_value(),
                          false,
                          "Zero-duration application throughput is not null");
    NS_TEST_ASSERT_MSG_EQ(noDuration.phyStats.channelUtilizationPercent.has_value(),
                          false,
                          "Zero-duration utilization is not null");

    const double largestDurationMs = 0x1.0624dd2f1a9fbp+53;
    NS_TEST_ASSERT_MSG_EQ(ConvertExperimentDurationMsToUs(largestDurationMs),
                          9223372036854774000LL,
                          "Largest representable duration was not converted exactly");
    bool overflowRejected = false;
    try
    {
        (void)ConvertExperimentDurationMsToUs(
            std::nextafter(largestDurationMs, std::numeric_limits<double>::infinity()));
    }
    catch (const std::overflow_error&)
    {
        overflowRejected = true;
    }
    NS_TEST_ASSERT_MSG_EQ(overflowRejected, true, "Overflowing duration was not rejected");
    bool nonFiniteRejected = false;
    try
    {
        (void)ConvertExperimentDurationMsToUs(std::numeric_limits<double>::infinity());
    }
    catch (const std::invalid_argument&)
    {
        nonFiniteRejected = true;
    }
    NS_TEST_ASSERT_MSG_EQ(nonFiniteRejected, true, "Non-finite duration was not rejected");

    LocalEntityWindowAccumulator largeDurationRaw;
    auto& largeTransmission = largeDurationRaw.deviceTransmission.uplink;
    largeTransmission.estimatedMatchedTcpPayloadBytes = 8;
    largeTransmission.matchedPacketCount = 1;
    largeTransmission.transmissionDurationUs.count = 1;
    largeTransmission.transmissionDurationUs.sum = 9223372036854775808.0L;
    largeTransmission.transmissionDurationUs.sumSquares =
        largeTransmission.transmissionDurationUs.sum * largeTransmission.transmissionDurationUs.sum;
    largeTransmission.transmissionDurationUs.minimum = 9223372036854775808.0;
    largeTransmission.transmissionDurationUs.maximum = 9223372036854775808.0;
    const auto largeDurationOutput =
        FinalizeEntityStatistics(largeDurationRaw, 1, *statistics.m_state).generalStats.uplink;
    NS_TEST_ASSERT_MSG_EQ(largeDurationOutput.totalTransmissionDurationUs,
                          uint64_t{1} << 63,
                          "Large raw duration did not retain its unsigned value");
    NS_TEST_ASSERT_MSG_EQ(largeDurationOutput.effectiveThroughputMbps.has_value(),
                          true,
                          "Positive uint64 duration produced a null rate");
    NS_TEST_ASSERT_MSG_EQ_TOL(largeDurationOutput.effectiveThroughputMbps.value(),
                              64.0 / 9223372036854775808.0,
                              1e-30,
                              "Large uint64 duration produced the wrong rate");
}

std::vector<TestCase*>
CreateExperimentSummaryTestCases()
{
    return {new ExperimentSummaryTestCase, new ExperimentOverallTestCase};
}
