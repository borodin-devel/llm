#include "../examples/traffic-coordinator.h"
#include "../examples/wifi-statistics-internal.h"
#include "llm-test-suite.h"

#include "ns3/ap-generator.h"
#include "ns3/network-module.h"

#include <optional>
#include <vector>

using namespace ns3;

namespace
{

/**
 * Open a deterministic experiment epoch for one test.
 *
 * @param coordinator Test traffic coordinator.
 * @return Experiment epoch in microseconds.
 */
int64_t
OpenExperiment(TrafficCoordinator& coordinator)
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
    return coordinator.GetExperimentStartUs();
}

/**
 * Register one test BSS in exact and transitional lookup state.
 *
 * @param statistics Test statistics state.
 */
void
RegisterEntities(WifiStatisticsState& statistics)
{
    statistics.entityRegistry.RegisterAccessPoint(0, 10, "AP0", "10.1.0.1");
    statistics.entityRegistry.RegisterStation(0, 0, 20, "AP0/STA0", "10.1.0.2");
    statistics.stationIpsByBss = {{"10.1.0.2"}};
    statistics.bssByApIp = {{"10.1.0.1", 0}};
    statistics.bssByStationIp = {{"10.1.0.2", 0}};
}

/**
 * @ingroup tests
 *
 * Verify tagged MPDU identity, direction, peer, and delay attribution.
 */
class ExperimentPhyMpduAttributionTestCase : public TestCase
{
  public:
    ExperimentPhyMpduAttributionTestCase();

  private:
    void DoRun() override;
};

ExperimentPhyMpduAttributionTestCase::ExperimentPhyMpduAttributionTestCase()
    : TestCase("attribute tagged PHY MPDU attempts by window and peer")
{
}

void
ExperimentPhyMpduAttributionTestCase::DoRun()
{
    TrafficCoordinator coordinator(30.0, 30.0);
    const int64_t epochUs = OpenExperiment(coordinator);
    WifiStatisticsState statistics(coordinator, 10);
    RegisterEntities(statistics);

    const std::vector<PhyTaggedPayloadSpan> uplinkSpans{
        {"10.1.0.2", "10.1.0.1", epochUs + 1000, 100},
        {"10.1.0.2", "10.1.0.1", epochUs + 1500, 50},
    };
    const PhyMpduKey uplinkIdentity{
        20,
        "00:00:00:00:00:01",
        "00:00:00:00:00:02",
        7,
        0,
        99,
    };
    RecordPhyMpduAttempt(statistics, 20, epochUs + 3000, 500, uplinkSpans, uplinkIdentity);
    RecordPhyMpduAttempt(statistics, 20, epochUs + 4000, 500, uplinkSpans, uplinkIdentity);

    const auto& station = statistics.unifiedWindows.at(0).at(20);
    const auto& stationUplink = station.phy.uplink;
    NS_TEST_ASSERT_MSG_EQ(stationUplink.taggedPayloadBytes,
                          300,
                          "STA attempted payload did not include retry bytes");
    NS_TEST_ASSERT_MSG_EQ(stationUplink.uniqueTaggedPayloadBytes,
                          150,
                          "STA unique payload included a retry");
    NS_TEST_ASSERT_MSG_EQ(stationUplink.taggedMpduCount,
                          2,
                          "Complete MPDU count was repeated per tagged span");
    NS_TEST_ASSERT_MSG_EQ(stationUplink.completeTaggedMpduBytes,
                          1000,
                          "Complete MPDU bytes were repeated per tagged span");
    NS_TEST_ASSERT_MSG_EQ(stationUplink.retransmissionCount,
                          1,
                          "Repeated MPDU identity was not a retransmission");
    NS_TEST_ASSERT_MSG_EQ(stationUplink.peersByNodeId.size(), 1, "Wrong STA PHY peer count");
    const auto& stationPeer = stationUplink.peersByNodeId.at(10);
    NS_TEST_ASSERT_MSG_EQ(stationPeer.taggedPayloadBytes, 300, "Wrong STA peer attempted bytes");
    NS_TEST_ASSERT_MSG_EQ(stationPeer.uniqueTaggedPayloadBytes, 150, "Wrong STA peer unique bytes");
    NS_TEST_ASSERT_MSG_EQ(stationPeer.retransmissionCount, 1, "Wrong STA peer retry count");
    NS_TEST_ASSERT_MSG_EQ(station.applicationToPhyDelayUs.uplink.count,
                          2,
                          "Application-to-PHY delay was repeated on retry");
    NS_TEST_ASSERT_MSG_EQ(station.applicationToPhyDelayUs.uplink.sum,
                          3500.0L,
                          "Wrong STA uplink application-to-PHY delay");
    NS_TEST_ASSERT_MSG_EQ(station.applicationToPhyDelayUs.downlink.count,
                          0,
                          "Uplink delay was recorded as downlink");

    const auto& accessPoint = statistics.unifiedWindows.at(0).at(10);
    const auto& accessPointUplink = accessPoint.phy.uplink;
    NS_TEST_ASSERT_MSG_EQ(accessPointUplink.taggedPayloadBytes,
                          300,
                          "STA uplink was not attributed to its AP parent");
    NS_TEST_ASSERT_MSG_EQ(accessPointUplink.taggedMpduCount,
                          2,
                          "AP parent MPDU count was repeated per span");
    NS_TEST_ASSERT_MSG_EQ(accessPointUplink.peersByNodeId.at(20).retransmissionCount,
                          1,
                          "AP parent peer retry attribution is wrong");
    NS_TEST_ASSERT_MSG_EQ(accessPoint.applicationToPhyDelayUs.uplink.count,
                          0,
                          "Sender delay was copied to the traffic peer");

    const std::vector<PhyTaggedPayloadSpan> downlinkSpans{
        {"10.1.0.1", "10.1.0.2", epochUs + 11000, 80},
    };
    const PhyMpduKey downlinkIdentity{
        10,
        "00:00:00:00:00:02",
        "00:00:00:00:00:01",
        8,
        0,
        100,
    };
    RecordPhyMpduAttempt(statistics, 10, epochUs + 12000, 300, downlinkSpans, downlinkIdentity);

    const auto& secondWindowAp = statistics.unifiedWindows.at(1).at(10);
    const auto& secondWindowSta = statistics.unifiedWindows.at(1).at(20);
    NS_TEST_ASSERT_MSG_EQ(secondWindowAp.phy.downlink.taggedPayloadBytes,
                          80,
                          "Wrong AP downlink payload");
    NS_TEST_ASSERT_MSG_EQ(secondWindowAp.phy.downlink.peersByNodeId.at(20).taggedPayloadBytes,
                          80,
                          "Wrong AP destination STA detail");
    NS_TEST_ASSERT_MSG_EQ(secondWindowSta.phy.downlink.taggedPayloadBytes,
                          80,
                          "AP downlink was not attributed to its destination STA");
    NS_TEST_ASSERT_MSG_EQ(secondWindowSta.phy.downlink.peersByNodeId.at(10).taggedPayloadBytes,
                          80,
                          "Wrong STA source AP detail");
    NS_TEST_ASSERT_MSG_EQ(secondWindowAp.applicationToPhyDelayUs.downlink.count,
                          1,
                          "AP downlink sender delay was not recorded");
    NS_TEST_ASSERT_MSG_EQ(secondWindowSta.applicationToPhyDelayUs.downlink.count,
                          0,
                          "AP sender delay was copied to the destination STA");

    NS_TEST_ASSERT_MSG_EQ(statistics.nodeSeconds.at(20).at(0).phyPayloadBytes,
                          300,
                          "Transitional PHY payload write changed");
    NS_TEST_ASSERT_MSG_EQ(statistics.nodeSeconds.at(20).at(0).phyRetransmissions,
                          1,
                          "Transitional PHY retry write changed");
    NS_TEST_ASSERT_MSG_EQ(statistics.nodeSeconds.at(20).at(0).phyUniquePayloadBytes,
                          150,
                          "Transitional unique PHY payload write changed");
    NS_TEST_ASSERT_MSG_EQ(statistics.nodeSeconds.at(20).at(0).phyMpduBytes,
                          1000,
                          "Transitional complete MPDU write changed");
    NS_TEST_ASSERT_MSG_EQ(statistics.nodeSeconds.at(20).at(0).appToPhy.count,
                          2,
                          "Transitional application-to-PHY delay write changed");

    Simulator::Destroy();
}

/**
 * @ingroup tests
 *
 * Verify PPDU attempt rates use allocated airtime and retain peer attribution.
 */
class ExperimentPhyRateTestCase : public TestCase
{
  public:
    ExperimentPhyRateTestCase();

  private:
    void DoRun() override;
};

ExperimentPhyRateTestCase::ExperimentPhyRateTestCase()
    : TestCase("weight configured-window PHY rates by allocated airtime")
{
}

void
ExperimentPhyRateTestCase::DoRun()
{
    TrafficCoordinator coordinator(30.0, 30.0);
    const int64_t epochUs = OpenExperiment(coordinator);
    WifiStatisticsState statistics(coordinator, 10);
    RegisterEntities(statistics);

    RecordPhyRateAttempt(statistics, epochUs + 2000, "10.1.0.2", "10.1.0.1", 10e6, 100.0L);
    RecordPhyRateAttempt(statistics, epochUs + 3000, "10.1.0.2", "10.1.0.1", 20e6, 300.0L);

    const auto& station = statistics.unifiedWindows.at(0).at(20).phy.uplink;
    const auto& accessPoint = statistics.unifiedWindows.at(0).at(10).phy.uplink;
    NS_TEST_ASSERT_MSG_EQ(station.transmissionAttemptCount, 2, "Wrong STA PPDU attempt count");
    NS_TEST_ASSERT_MSG_EQ(station.transmissionAirtimeUs, 400.0L, "Wrong STA allocated airtime");
    NS_TEST_ASSERT_MSG_EQ(station.dataRateBpsUs,
                          7000000000.0L,
                          "Wrong literal rate-airtime product");
    const std::optional<double> average = CalculateAveragePhyDataRateMbps(station);
    NS_TEST_ASSERT_MSG_EQ(average.has_value(), true, "Positive airtime produced no average rate");
    NS_TEST_ASSERT_MSG_EQ_TOL(*average, 17.5, 1e-12, "Wrong airtime-weighted PHY rate");
    NS_TEST_ASSERT_MSG_EQ(station.peersByNodeId.at(10).transmissionAttemptCount,
                          2,
                          "Wrong STA peer PPDU attempt count");
    NS_TEST_ASSERT_MSG_EQ(accessPoint.peersByNodeId.at(20).transmissionAirtimeUs,
                          400.0L,
                          "Wrong AP parent peer airtime");
    NS_TEST_ASSERT_MSG_EQ(statistics.phyWindows.contains(0),
                          true,
                          "Transitional PHY rate window was not written");
    if (!statistics.phyWindows.contains(0))
    {
        Simulator::Destroy();
        return;
    }
    const auto& legacy = statistics.phyWindows.at(0).at(0).upPhyRates.at("10.1.0.2");
    NS_TEST_ASSERT_MSG_EQ(legacy.txAttempts, 2, "Transitional PHY rate attempts changed");
    NS_TEST_ASSERT_MSG_EQ_TOL(legacy.AverageMbps(),
                              17.5,
                              1e-12,
                              "Transitional weighted PHY rate changed");

    const PhyPeerAccumulator noAirtime;
    NS_TEST_ASSERT_MSG_EQ(CalculateAveragePhyDataRateMbps(noAirtime).has_value(),
                          false,
                          "No airtime must produce a null average later");

    Simulator::Destroy();
}

/**
 * @ingroup tests
 *
 * Verify local PHY busy time is clipped and split at configured boundaries.
 */
class ExperimentPhyBusyTimeTestCase : public TestCase
{
  public:
    ExperimentPhyBusyTimeTestCase();

  private:
    void DoRun() override;
};

ExperimentPhyBusyTimeTestCase::ExperimentPhyBusyTimeTestCase()
    : TestCase("split local PHY busy time across configured windows")
{
}

void
ExperimentPhyBusyTimeTestCase::DoRun()
{
    TrafficCoordinator coordinator(30.0, 30.0);
    const int64_t epochUs = OpenExperiment(coordinator);
    WifiStatisticsState statistics(coordinator, 10);
    RegisterEntities(statistics);

    RecordPhyBusyInterval(statistics, 10, epochUs + 8000, 14000);
    RecordPhyBusyInterval(statistics, 20, epochUs, 5000);

    NS_TEST_ASSERT_MSG_EQ(statistics.unifiedWindows.at(0).at(10).phy.busyTimeUs,
                          2000,
                          "Wrong first-window AP busy overlap");
    NS_TEST_ASSERT_MSG_EQ(statistics.unifiedWindows.at(1).at(10).phy.busyTimeUs,
                          10000,
                          "Wrong middle-window AP busy overlap");
    NS_TEST_ASSERT_MSG_EQ(statistics.unifiedWindows.at(2).at(10).phy.busyTimeUs,
                          2000,
                          "Wrong final-window AP busy overlap");
    NS_TEST_ASSERT_MSG_EQ(statistics.unifiedWindows.at(0).at(20).phy.busyTimeUs,
                          5000,
                          "Wrong local STA busy time");
    NS_TEST_ASSERT_MSG_EQ(statistics.unifiedWindows.at(0).at(10).phy.busyTimeUs,
                          2000,
                          "STA busy time was summed into the AP category");
    NS_TEST_ASSERT_MSG_EQ(statistics.nodeSeconds.at(10).at(0).phyBusyUs,
                          14000,
                          "Transitional fixed-second busy split changed");

    Simulator::Destroy();
}

} // namespace

std::vector<TestCase*>
CreateExperimentPhyTestCases()
{
    return {new ExperimentPhyMpduAttributionTestCase,
            new ExperimentPhyRateTestCase,
            new ExperimentPhyBusyTimeTestCase};
}
