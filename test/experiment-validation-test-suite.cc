#include "../examples/experiment-window-output.h"
#include "../examples/traffic-coordinator.h"
#include "../examples/wifi-statistics-internal.h"
#include "../examples/wifi-statistics.h"
#include "llm-test-suite.h"

#include "ns3/ap-generator.h"
#include "ns3/network-module.h"

#include <cmath>
#include <functional>
#include <limits>
#include <string>
#include <vector>

using namespace ns3;

/** @ingroup tests Verify every raw validation flag can fail independently. */
class ExperimentValidationTestCase : public TestCase
{
  public:
    ExperimentValidationTestCase();

  private:
    void DoRun() override;

    /**
     * Assert exactly one named validation flag is false.
     *
     * @param validation Validation result to inspect.
     * @param expected Name of the expected false flag.
     */
    void AssertOnlyFalse(const ExperimentValidationOutput& validation, const std::string& expected);
};

ExperimentValidationTestCase::ExperimentValidationTestCase()
    : TestCase("validate independent raw hierarchy invariants")
{
}

void
ExperimentValidationTestCase::AssertOnlyFalse(const ExperimentValidationOutput& validation,
                                              const std::string& expected)
{
    const std::vector<std::pair<std::string, bool>> flags = {
        {"inventory", validation.entityInventoryReferencesValid},
        {"agents", validation.appAgentTotalsConsistent},
        {"app-peers", validation.appPeerTotalsConsistent},
        {"mac-peers", validation.macPeerTotalsConsistent},
        {"phy-peers", validation.phyPeerTotalsConsistent},
        {"parent-child", validation.apStationSenderTotalsConsistent},
        {"overall", validation.overallMatchesWindows},
        {"unique-phy", validation.uniquePhyPayloadWithinTaggedPayload},
    };
    uint32_t falseCount = 0;
    for (const auto& [name, value] : flags)
    {
        falseCount += !value;
        NS_TEST_ASSERT_MSG_EQ(value, name != expected, "Unexpected validation result for " << name);
    }
    NS_TEST_ASSERT_MSG_EQ(falseCount, 1, "Mutation did not isolate one validation flag");
}

void
ExperimentValidationTestCase::DoRun()
{
    TrafficCoordinator coordinator(10.0, 10.0);
    WifiStatistics statistics(coordinator, 10);
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

    statistics.RegisterAccessPointIdentity(0, 10, "AP0", Ipv4Address("10.1.0.1"));
    statistics.RegisterStationIdentity(0, 0, 20, "STA0", Ipv4Address("10.1.0.2"));
    statistics.RegisterStationIdentity(0, 1, 21, "STA1", Ipv4Address("10.1.0.3"));
    auto& station = statistics.m_state->unifiedWindows[0][20];
    station.app.uplink.acceptedSendCount = 1;
    station.app.uplink.acceptedPayloadBytes = 100;
    station.app.uplink.agents["agent"].acceptedSendCount = 1;
    station.app.uplink.agents["agent"].acceptedPayloadBytes = 100;
    station.app.uplink.peersByNodeId[10].acceptedSendCount = 1;
    station.app.uplink.peersByNodeId[10].acceptedPayloadBytes = 100;
    station.mac.uplink.estimatedTransmitEventCount = 1;
    station.mac.uplink.estimatedTransmittedTcpPayloadBytes = 80;
    station.mac.uplink.transmitDropCount = 1;
    station.mac.uplink.transmitDropPacketBytes = 20;
    station.mac.uplink.mpduDropCount = 3;
    station.mac.uplink.mpduDropBytes = 350;
    station.mac.uplink.dataFailureCount = 5;
    station.mac.uplink.finalDataFailureCount = 3;
    station.mac.uplink.mpduDropsByReason[9] = 1;
    station.mac.uplink.mpduDropsByReason[7] = 1;
    station.mac.uplink.mpduDropsByReason[4] = 1;
    auto& macPeer = station.mac.uplink.peersByNodeId[10];
    macPeer.estimatedTransmitEventCount = 1;
    macPeer.estimatedTransmittedTcpPayloadBytes = 80;
    macPeer.mpduDropCount = 2;
    macPeer.mpduDropBytes = 300;
    macPeer.dataFailureCount = 3;
    macPeer.finalDataFailureCount = 1;
    macPeer.mpduDropsByReason[9] = 1;
    macPeer.mpduDropsByReason[4] = 1;
    station.phy.uplink.taggedPayloadBytes = 70;
    station.phy.uplink.uniqueTaggedPayloadBytes = 60;
    station.phy.uplink.transmissionAttemptCount = 1;
    station.phy.uplink.transmissionAirtimeUs = 10.0L;
    const long double bigRateProduct = std::ldexp(1.0L, std::numeric_limits<long double>::digits);
    constexpr long double smallRateProduct = 1.0L;
    station.phy.uplink.dataRateBpsUs = bigRateProduct;
    station.phy.uplink.peersByNodeId[10].taggedPayloadBytes = 70;
    station.phy.uplink.peersByNodeId[10].uniqueTaggedPayloadBytes = 60;
    station.phy.uplink.peersByNodeId[10].transmissionAttemptCount = 1;
    station.phy.uplink.peersByNodeId[10].transmissionAirtimeUs = 10.0L;
    station.phy.uplink.peersByNodeId[10].dataRateBpsUs = bigRateProduct;

    auto& secondStation = statistics.m_state->unifiedWindows[0][21];
    secondStation.phy.uplink.taggedPayloadBytes = 1;
    secondStation.phy.uplink.uniqueTaggedPayloadBytes = 1;
    secondStation.phy.uplink.transmissionAttemptCount = 1;
    secondStation.phy.uplink.transmissionAirtimeUs = 1.0L;
    secondStation.phy.uplink.dataRateBpsUs = smallRateProduct + smallRateProduct;
    secondStation.phy.uplink.peersByNodeId[10].taggedPayloadBytes = 1;
    secondStation.phy.uplink.peersByNodeId[10].uniqueTaggedPayloadBytes = 1;
    secondStation.phy.uplink.peersByNodeId[10].transmissionAttemptCount = 1;
    secondStation.phy.uplink.peersByNodeId[10].transmissionAirtimeUs = 1.0L;
    secondStation.phy.uplink.peersByNodeId[10].dataRateBpsUs = smallRateProduct + smallRateProduct;

    auto& accessPoint = statistics.m_state->unifiedWindows[0][10];
    accessPoint.app.uplink.receiveEventCount = 1;
    accessPoint.app.uplink.receivedPayloadBytes = 90;
    accessPoint.app.uplink.peersByNodeId[20].receiveEventCount = 1;
    accessPoint.app.uplink.peersByNodeId[20].receivedPayloadBytes = 90;
    accessPoint.app.downlink.acceptedSendCount = 1;
    accessPoint.app.downlink.acceptedPayloadBytes = 200;
    accessPoint.app.downlink.agents["downlink-agent"].acceptedSendCount = 1;
    accessPoint.app.downlink.agents["downlink-agent"].acceptedPayloadBytes = 200;
    accessPoint.app.downlink.peersByNodeId[20].acceptedSendCount = 1;
    accessPoint.app.downlink.peersByNodeId[20].acceptedPayloadBytes = 200;
    station.app.downlink.receiveEventCount = 1;
    station.app.downlink.receivedPayloadBytes = 50;
    station.app.downlink.peersByNodeId[10].receiveEventCount = 1;
    station.app.downlink.peersByNodeId[10].receivedPayloadBytes = 50;
    accessPoint.mac.uplink.estimatedReceiveEventCount = 1;
    accessPoint.mac.uplink.estimatedReceivedTcpPayloadBytes = 75;
    accessPoint.mac.uplink.peersByNodeId[20].estimatedReceiveEventCount = 1;
    accessPoint.mac.uplink.peersByNodeId[20].estimatedReceivedTcpPayloadBytes = 75;
    accessPoint.phy.busyTimeUs = 100;
    accessPoint.phy.uplink.taggedPayloadBytes = 71;
    accessPoint.phy.uplink.uniqueTaggedPayloadBytes = 61;
    accessPoint.phy.uplink.transmissionAttemptCount = 2;
    accessPoint.phy.uplink.transmissionAirtimeUs = 11.0L;
    long double eventOrderRateProduct = bigRateProduct;
    eventOrderRateProduct += smallRateProduct;
    eventOrderRateProduct += smallRateProduct;
    accessPoint.phy.uplink.dataRateBpsUs = eventOrderRateProduct;
    accessPoint.phy.uplink.peersByNodeId[20].taggedPayloadBytes = 70;
    accessPoint.phy.uplink.peersByNodeId[20].uniqueTaggedPayloadBytes = 60;
    accessPoint.phy.uplink.peersByNodeId[20].transmissionAttemptCount = 1;
    accessPoint.phy.uplink.peersByNodeId[20].transmissionAirtimeUs = 10.0L;
    accessPoint.phy.uplink.peersByNodeId[20].dataRateBpsUs = bigRateProduct;
    accessPoint.phy.uplink.peersByNodeId[21].taggedPayloadBytes = 1;
    accessPoint.phy.uplink.peersByNodeId[21].uniqueTaggedPayloadBytes = 1;
    accessPoint.phy.uplink.peersByNodeId[21].transmissionAttemptCount = 1;
    accessPoint.phy.uplink.peersByNodeId[21].transmissionAirtimeUs = 1.0L;
    accessPoint.phy.uplink.peersByNodeId[21].dataRateBpsUs = smallRateProduct + smallRateProduct;

    const UnifiedSummaryRawState base = BuildUnifiedSummaryRawState(*statistics.m_state);
    const auto& parentPhy = base.accessPointWindows.at(0).at(10).phy.uplink;
    const long double peerOrderRateProduct =
        parentPhy.peersByNodeId.at(20).dataRateBpsUs + parentPhy.peersByNodeId.at(21).dataRateBpsUs;
    NS_TEST_ASSERT_MSG_NE(parentPhy.dataRateBpsUs,
                          peerOrderRateProduct,
                          "Fixture did not change raw accumulation order");
    const ExperimentValidationOutput valid =
        ValidateUnifiedSummaryRawState(statistics.m_state->entityRegistry, base);
    NS_TEST_ASSERT_MSG_EQ(valid.entityInventoryReferencesValid, true, "Base inventory is invalid");
    NS_TEST_ASSERT_MSG_EQ(valid.appAgentTotalsConsistent, true, "Base agents are invalid");
    NS_TEST_ASSERT_MSG_EQ(valid.appPeerTotalsConsistent, true, "Base app peers are invalid");
    NS_TEST_ASSERT_MSG_EQ(valid.macPeerTotalsConsistent, true, "Base MAC peers are invalid");
    NS_TEST_ASSERT_MSG_EQ(valid.phyPeerTotalsConsistent, true, "Base PHY peers are invalid");
    NS_TEST_ASSERT_MSG_EQ(valid.apStationSenderTotalsConsistent,
                          true,
                          "Base parent/child totals are invalid");
    NS_TEST_ASSERT_MSG_EQ(valid.overallMatchesWindows, true, "Base overall is invalid");
    NS_TEST_ASSERT_MSG_EQ(valid.uniquePhyPayloadWithinTaggedPayload,
                          true,
                          "Base unique PHY total is invalid");

    auto Check = [&](const std::string& name,
                     const std::function<void(UnifiedSummaryRawState&)>& mutate) {
        UnifiedSummaryRawState copy = base;
        mutate(copy);
        AssertOnlyFalse(ValidateUnifiedSummaryRawState(statistics.m_state->entityRegistry, copy),
                        name);
    };

    Check("inventory",
          [](auto& raw) { raw.localWindows[0][999].app.uplink.acceptedPayloadBytes = 1; });
    Check("agents", [](auto& raw) {
        raw.stationWindows[0][20].app.uplink.agents["agent"].acceptedPayloadBytes = 101;
        raw.stationOverall[20].app.uplink.agents["agent"].acceptedPayloadBytes = 101;
    });
    Check("app-peers", [](auto& raw) {
        raw.stationWindows[0][20].app.uplink.peersByNodeId[10].acceptedPayloadBytes = 101;
        raw.stationOverall[20].app.uplink.peersByNodeId[10].acceptedPayloadBytes = 101;
    });
    Check("mac-peers", [](auto& raw) {
        raw.stationWindows[0][20].mac.uplink.peersByNodeId[10].estimatedTransmittedTcpPayloadBytes =
            81;
        raw.stationOverall[20].mac.uplink.peersByNodeId[10].estimatedTransmittedTcpPayloadBytes =
            81;
    });
    Check("mac-peers", [](auto& raw) {
        raw.stationWindows[0][20].mac.uplink.peersByNodeId[10].mpduDropCount = 3;
        raw.stationOverall[20].mac.uplink.peersByNodeId[10].mpduDropCount = 3;
    });
    Check("mac-peers", [](auto& raw) {
        raw.stationWindows[0][20].mac.uplink.peersByNodeId[10].mpduDropBytes = 351;
        raw.stationOverall[20].mac.uplink.peersByNodeId[10].mpduDropBytes = 351;
    });
    Check("mac-peers", [](auto& raw) {
        raw.stationWindows[0][20].mac.uplink.peersByNodeId[10].mpduDropsByReason[4] = 2;
        raw.stationOverall[20].mac.uplink.peersByNodeId[10].mpduDropsByReason[4] = 2;
    });
    Check("mac-peers", [](auto& raw) {
        raw.stationWindows[0][20].mac.uplink.peersByNodeId[10].dataFailureCount = 6;
        raw.stationOverall[20].mac.uplink.peersByNodeId[10].dataFailureCount = 6;
    });
    Check("mac-peers", [](auto& raw) {
        raw.stationWindows[0][20].mac.uplink.peersByNodeId[10].finalDataFailureCount = 4;
        raw.stationOverall[20].mac.uplink.peersByNodeId[10].finalDataFailureCount = 4;
    });
    Check("phy-peers", [](auto& raw) {
        raw.stationWindows[0][20].phy.uplink.peersByNodeId[10].taggedPayloadBytes = 71;
        raw.stationOverall[20].phy.uplink.peersByNodeId[10].taggedPayloadBytes = 71;
    });
    const long double materialRatePerturbation =
        256.0L * std::numeric_limits<long double>::epsilon() * bigRateProduct;
    Check("phy-peers", [materialRatePerturbation](auto& raw) {
        raw.stationWindows[0][20].phy.uplink.peersByNodeId[10].dataRateBpsUs +=
            materialRatePerturbation;
        raw.stationOverall[20].phy.uplink.peersByNodeId[10].dataRateBpsUs +=
            materialRatePerturbation;
    });
    Check("parent-child", [](auto& raw) {
        raw.accessPointWindows[0][10].app.uplink.acceptedPayloadBytes = 101;
        raw.accessPointWindows[0][10].app.uplink.agents["agent"].acceptedPayloadBytes = 101;
        raw.accessPointOverall[10].app.uplink.acceptedPayloadBytes = 101;
        raw.accessPointOverall[10].app.uplink.agents["agent"].acceptedPayloadBytes = 101;
    });
    Check("parent-child", [](auto& raw) {
        raw.accessPointWindows[0][10].app.downlink.acceptedPayloadBytes = 201;
        raw.accessPointWindows[0][10].app.downlink.agents["downlink-agent"].acceptedPayloadBytes =
            201;
        raw.accessPointWindows[0][10].app.downlink.peersByNodeId[20].acceptedPayloadBytes = 201;
        raw.accessPointOverall[10].app.downlink.acceptedPayloadBytes = 201;
        raw.accessPointOverall[10].app.downlink.agents["downlink-agent"].acceptedPayloadBytes = 201;
        raw.accessPointOverall[10].app.downlink.peersByNodeId[20].acceptedPayloadBytes = 201;
    });
    Check("parent-child", [](auto& raw) {
        raw.accessPointWindows[0][10].app.uplink.receivedPayloadBytes = 91;
        raw.accessPointWindows[0][10].app.uplink.peersByNodeId[20].receivedPayloadBytes = 91;
        raw.accessPointOverall[10].app.uplink.receivedPayloadBytes = 91;
        raw.accessPointOverall[10].app.uplink.peersByNodeId[20].receivedPayloadBytes = 91;
    });
    Check("parent-child", [](auto& raw) {
        raw.accessPointWindows[0][10].app.downlink.receivedPayloadBytes = 51;
        raw.accessPointWindows[0][10].app.downlink.peersByNodeId[20].receivedPayloadBytes = 51;
        raw.accessPointOverall[10].app.downlink.receivedPayloadBytes = 51;
        raw.accessPointOverall[10].app.downlink.peersByNodeId[20].receivedPayloadBytes = 51;
    });
    Check("parent-child", [](auto& raw) {
        raw.accessPointWindows[0][10].phy.busyTimeUs = 101;
        raw.accessPointOverall[10].phy.busyTimeUs = 101;
    });
    Check("overall", [](auto& raw) {
        raw.stationOverall[20].deviceTransmission.downlink.estimatedTransmittedTcpPayloadBytes = 1;
    });
    Check("unique-phy", [](auto& raw) {
        raw.localWindows[0][20].phy.uplink.uniqueTaggedPayloadBytes = 71;
        raw.localWindows[0][20].phy.uplink.peersByNodeId[10].uniqueTaggedPayloadBytes = 71;
        raw.localWindows[0][10].phy.uplink.uniqueTaggedPayloadBytes = 72;
        raw.localWindows[0][10].phy.uplink.peersByNodeId[20].uniqueTaggedPayloadBytes = 71;
        raw.stationWindows[0][20].phy.uplink.uniqueTaggedPayloadBytes = 71;
        raw.stationWindows[0][20].phy.uplink.peersByNodeId[10].uniqueTaggedPayloadBytes = 71;
        raw.stationOverall[20].phy.uplink.uniqueTaggedPayloadBytes = 71;
        raw.stationOverall[20].phy.uplink.peersByNodeId[10].uniqueTaggedPayloadBytes = 71;
        raw.accessPointWindows[0][10].phy.uplink.uniqueTaggedPayloadBytes = 72;
        raw.accessPointWindows[0][10].phy.uplink.peersByNodeId[20].uniqueTaggedPayloadBytes = 71;
        raw.accessPointOverall[10].phy.uplink.uniqueTaggedPayloadBytes = 72;
        raw.accessPointOverall[10].phy.uplink.peersByNodeId[20].uniqueTaggedPayloadBytes = 71;
    });

    Simulator::Destroy();
}

std::vector<TestCase*>
CreateExperimentValidationTestCases()
{
    return {new ExperimentValidationTestCase};
}
