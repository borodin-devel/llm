#include "../examples/traffic-coordinator.h"
#include "../examples/wifi-statistics-internal.h"
#include "llm-test-suite.h"

#include "ns3/ap-generator.h"
#include "ns3/network-module.h"

#include <cstdint>
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
 * Register one test AP and station.
 *
 * @param statistics Test statistics state.
 */
void
RegisterEntities(WifiStatisticsState& statistics)
{
    statistics.entityRegistry.RegisterAccessPoint(0, 10, "AP0", "10.1.0.1");
    statistics.entityRegistry.RegisterStation(0, 0, 20, "AP0/STA0", "10.1.0.2");
}

/**
 * @ingroup tests
 *
 * Verify device observations retain causal window ownership and ordered positive matches.
 */
class ExperimentDeviceMatchingTestCase : public TestCase
{
  public:
    ExperimentDeviceMatchingTestCase();

  private:
    void DoRun() override;
};

ExperimentDeviceMatchingTestCase::ExperimentDeviceMatchingTestCase()
    : TestCase("match device TCP payload observations in transmit windows")
{
}

void
ExperimentDeviceMatchingTestCase::DoRun()
{
    TrafficCoordinator coordinator(30.0, 30.0);
    const int64_t epochUs = OpenExperiment(coordinator);
    WifiStatisticsState statistics(coordinator, 10);
    RegisterEntities(statistics);

    const ParsedDeviceTcpPayload crossWindowUplink{"10.1.0.2", 9000, "10.1.0.1", 10000, 1000};
    RecordParsedDeviceTransmit(statistics, epochUs + 9900, crossWindowUplink);
    RecordParsedDeviceReceive(statistics, epochUs + 10100, crossWindowUplink);

    const ParsedDeviceTcpPayload downlink{"10.1.0.1", 10000, "10.1.0.2", 9000, 600};
    RecordParsedDeviceTransmit(statistics, epochUs + 11000, downlink);
    RecordParsedDeviceReceive(statistics, epochUs + 11250, downlink);

    const ParsedDeviceTcpPayload largeUnmatched{"10.1.0.2", 9001, "10.1.0.1", 10000, 3000000000U};
    RecordParsedDeviceTransmit(statistics, epochUs + 2000, largeUnmatched);

    const ParsedDeviceTcpPayload orderedIdentical{"10.1.0.2", 9002, "10.1.0.1", 10000, 200};
    RecordParsedDeviceTransmit(statistics, epochUs + 3000, orderedIdentical);
    RecordParsedDeviceTransmit(statistics, epochUs + 5000, orderedIdentical);
    RecordParsedDeviceTransmit(statistics, epochUs + 7000, orderedIdentical);
    RecordParsedDeviceReceive(statistics, epochUs + 3000, orderedIdentical);
    RecordParsedDeviceReceive(statistics, epochUs + 4900, orderedIdentical);
    RecordParsedDeviceReceive(statistics, epochUs + 7600, orderedIdentical);

    FinalizeDeviceStatistics(statistics);
    FinalizeDeviceStatistics(statistics);

    const auto& windows = statistics.unifiedWindows;
    const auto& stationUplink = windows.at(0).at(20).deviceTransmission.uplink;
    NS_TEST_ASSERT_MSG_EQ(stationUplink.estimatedTransmittedTcpPayloadBytes,
                          3000001600ULL,
                          "Device TX bytes were not accumulated with 64-bit arithmetic");
    NS_TEST_ASSERT_MSG_EQ(stationUplink.estimatedMatchedTcpPayloadBytes,
                          1200,
                          "Wrong positive matched payload bytes");
    NS_TEST_ASSERT_MSG_EQ(stationUplink.matchedPacketCount,
                          2,
                          "Zero or negative duration was accepted as a match");
    NS_TEST_ASSERT_MSG_EQ(stationUplink.transmissionDurationUs.count,
                          2,
                          "Finalization was not idempotent");
    NS_TEST_ASSERT_MSG_EQ(stationUplink.transmissionDurationUs.sum,
                          800.0L,
                          "Identical flows were not paired in observation order");
    NS_TEST_ASSERT_MSG_EQ(stationUplink.transmissionDurationUs.minimum,
                          200.0,
                          "Wrong minimum device duration");
    NS_TEST_ASSERT_MSG_EQ(stationUplink.transmissionDurationUs.maximum,
                          600.0,
                          "Wrong maximum device duration");
    NS_TEST_ASSERT_MSG_EQ(windows.at(1).at(20).deviceTransmission.uplink.matchedPacketCount,
                          0,
                          "Cross-window match was assigned to its receive window");

    const auto& accessPointDownlink = windows.at(1).at(10).deviceTransmission.downlink;
    NS_TEST_ASSERT_MSG_EQ(accessPointDownlink.estimatedTransmittedTcpPayloadBytes,
                          600,
                          "Wrong AP downlink device bytes");
    NS_TEST_ASSERT_MSG_EQ(accessPointDownlink.estimatedMatchedTcpPayloadBytes,
                          600,
                          "Wrong AP downlink matched bytes");
    NS_TEST_ASSERT_MSG_EQ(accessPointDownlink.transmissionDurationUs.sum,
                          250.0L,
                          "Wrong AP downlink duration");

    const auto& stationMacUplink = windows.at(0).at(20).mac.uplink;
    NS_TEST_ASSERT_MSG_EQ(stationMacUplink.estimatedTransmitEventCount,
                          5,
                          "Wrong STA uplink MAC transmit count");
    NS_TEST_ASSERT_MSG_EQ(stationMacUplink.estimatedTransmittedTcpPayloadBytes,
                          3000001600ULL,
                          "Wrong STA uplink MAC transmit bytes");
    NS_TEST_ASSERT_MSG_EQ(stationMacUplink.peersByNodeId.size(),
                          1,
                          "Wrong STA uplink MAC peer count");
    NS_TEST_ASSERT_MSG_EQ(stationMacUplink.peersByNodeId.at(10).estimatedTransmittedTcpPayloadBytes,
                          3000001600ULL,
                          "Wrong STA-to-AP peer transmit bytes");

    const auto& firstWindowApMacUplink = windows.at(0).at(10).mac.uplink;
    NS_TEST_ASSERT_MSG_EQ(firstWindowApMacUplink.estimatedReceiveEventCount,
                          3,
                          "Wrong first-window AP receive count");
    NS_TEST_ASSERT_MSG_EQ(firstWindowApMacUplink.estimatedReceivedTcpPayloadBytes,
                          600,
                          "Wrong first-window AP receive bytes");
    const auto& secondWindowApMacUplink = windows.at(1).at(10).mac.uplink;
    NS_TEST_ASSERT_MSG_EQ(secondWindowApMacUplink.estimatedReceiveEventCount,
                          1,
                          "Device RX did not use its own event window");
    NS_TEST_ASSERT_MSG_EQ(
        secondWindowApMacUplink.peersByNodeId.at(20).estimatedReceivedTcpPayloadBytes,
        1000,
        "Wrong AP receive peer bytes");

    const auto& apMacDownlink = windows.at(1).at(10).mac.downlink;
    const auto& stationMacDownlink = windows.at(1).at(20).mac.downlink;
    NS_TEST_ASSERT_MSG_EQ(apMacDownlink.estimatedTransmitEventCount,
                          1,
                          "AP transmit did not map to downlink");
    NS_TEST_ASSERT_MSG_EQ(apMacDownlink.peersByNodeId.at(20).estimatedTransmitEventCount,
                          1,
                          "AP downlink peer was not resolved");
    NS_TEST_ASSERT_MSG_EQ(stationMacDownlink.estimatedReceiveEventCount,
                          1,
                          "STA receive did not map to downlink");
    NS_TEST_ASSERT_MSG_EQ(stationMacDownlink.peersByNodeId.at(10).estimatedReceiveEventCount,
                          1,
                          "STA downlink peer was not resolved");

    const TransmissionSummary summary = BuildTransmissionSummary(statistics);
    NS_TEST_ASSERT_MSG_EQ(summary.senders.size(), 2, "Wrong transitional sender count");
    NS_TEST_ASSERT_MSG_EQ(summary.senders.at(0).senderIpv4,
                          "10.1.0.1",
                          "Transitional senders are not ordered");
    NS_TEST_ASSERT_MSG_EQ(summary.senders.at(1).transmittedPayloadBytes,
                          3000001600ULL,
                          "Transitional adapter truncated transmitted bytes");
    NS_TEST_ASSERT_MSG_EQ(summary.senders.at(1).matchedPacketCount,
                          2,
                          "Transitional adapter lost matched packets");
    NS_TEST_ASSERT_MSG_EQ(summary.senders.at(1).totalTransmissionDurationUs,
                          800,
                          "Transitional adapter lost durations");

    Simulator::Destroy();
}

/**
 * @ingroup tests
 *
 * Verify MAC transmitter events use configured windows, entity roles, and exact totals.
 */
class ExperimentMacEventTestCase : public TestCase
{
  public:
    ExperimentMacEventTestCase();

  private:
    void DoRun() override;
};

ExperimentMacEventTestCase::ExperimentMacEventTestCase()
    : TestCase("collect MAC drops and failures in configured windows")
{
}

void
ExperimentMacEventTestCase::DoRun()
{
    TrafficCoordinator coordinator(30.0, 30.0);
    const int64_t epochUs = OpenExperiment(coordinator);
    WifiStatisticsState statistics(coordinator, 10);
    RegisterEntities(statistics);

    RecordMacTransmitDrop(statistics, 10, epochUs + 9999, 500);
    RecordMacMpduDrop(statistics, 10, epochUs + 2000, 9, 700);
    RecordMacMpduDrop(statistics, 10, epochUs + 3000, 4, 300);
    RecordMacDataFailure(statistics, 10, epochUs + 4000, false);
    RecordMacDataFailure(statistics, 10, epochUs + 5000, true);

    RecordMacTransmitDrop(statistics, 20, epochUs + 10000, 600);
    RecordMacDataFailure(statistics, 20, epochUs + 12000, false);
    RecordMacDataFailure(statistics, 20, epochUs + 13000, true);
    RecordMacTransmitDrop(statistics, 20, epochUs + 30000, 999);

    const auto& accessPoint = statistics.unifiedWindows.at(0).at(10).mac.downlink;
    NS_TEST_ASSERT_MSG_EQ(accessPoint.transmitDropCount, 1, "Wrong AP transmit-drop count");
    NS_TEST_ASSERT_MSG_EQ(accessPoint.transmitDropPacketBytes,
                          500,
                          "Wrong AP transmit-drop packet bytes");
    NS_TEST_ASSERT_MSG_EQ(accessPoint.mpduDropCount, 2, "Wrong AP MPDU-drop count");
    NS_TEST_ASSERT_MSG_EQ(accessPoint.mpduDropBytes, 1000, "Wrong AP MPDU-drop bytes");
    NS_TEST_ASSERT_MSG_EQ(accessPoint.dataFailureCount, 1, "Wrong AP data-failure count");
    NS_TEST_ASSERT_MSG_EQ(accessPoint.finalDataFailureCount,
                          1,
                          "Wrong AP final-data-failure count");
    NS_TEST_ASSERT_MSG_EQ(accessPoint.mpduDropsByReason.size(), 2, "Wrong reason count");
    NS_TEST_ASSERT_MSG_EQ(accessPoint.mpduDropsByReason.begin()->first,
                          4,
                          "MAC reasons are not numerically ordered");
    NS_TEST_ASSERT_MSG_EQ(accessPoint.mpduDropsByReason.at(4), 1, "Wrong reason 4 count");
    NS_TEST_ASSERT_MSG_EQ(accessPoint.mpduDropsByReason.at(9), 1, "Wrong reason 9 count");
    NS_TEST_ASSERT_MSG_EQ(statistics.unifiedWindows.at(0).at(10).mac.uplink.transmitDropCount,
                          0,
                          "AP transmitter event mapped to uplink");

    const auto& station = statistics.unifiedWindows.at(1).at(20).mac.uplink;
    NS_TEST_ASSERT_MSG_EQ(station.transmitDropCount, 1, "Wrong STA transmit-drop count");
    NS_TEST_ASSERT_MSG_EQ(station.transmitDropPacketBytes, 600, "Wrong STA transmit-drop bytes");
    NS_TEST_ASSERT_MSG_EQ(station.dataFailureCount, 1, "Wrong STA data-failure count");
    NS_TEST_ASSERT_MSG_EQ(station.finalDataFailureCount, 1, "Wrong STA final-data-failure count");
    NS_TEST_ASSERT_MSG_EQ(statistics.unifiedWindows.at(1).at(20).mac.downlink.transmitDropCount,
                          0,
                          "STA transmitter event mapped to downlink");
    NS_TEST_ASSERT_MSG_EQ(statistics.unifiedWindows.contains(3),
                          false,
                          "Out-of-duration MAC event created a window");

    const auto& legacyAccessPoint = statistics.nodeSeconds.at(10).at(0);
    NS_TEST_ASSERT_MSG_EQ(legacyAccessPoint.macTxDrops,
                          1,
                          "Legacy cross-layer transmit drops were not retained");
    NS_TEST_ASSERT_MSG_EQ(legacyAccessPoint.macMpduDropBytes,
                          1000,
                          "Legacy cross-layer MPDU bytes were not retained");
    NS_TEST_ASSERT_MSG_EQ(legacyAccessPoint.macDataFailures,
                          1,
                          "Legacy cross-layer failures were not retained");
    NS_TEST_ASSERT_MSG_EQ(legacyAccessPoint.macFinalDataFailures,
                          1,
                          "Legacy cross-layer final failures were not retained");

    Simulator::Destroy();
}

} // namespace

std::vector<TestCase*>
CreateExperimentDeviceMacTestCases()
{
    return {new ExperimentDeviceMatchingTestCase, new ExperimentMacEventTestCase};
}
