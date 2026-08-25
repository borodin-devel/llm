#include "../examples/traffic-coordinator.h"
#include "../examples/wifi-statistics-internal.h"
#include "../examples/wifi-statistics.h"
#include "llm-test-suite.h"

#include "ns3/ap-generator.h"
#include "ns3/network-module.h"
#include "ns3/sta-llm-generator.h"
#include "ns3/traffic-sink.h"

#include <cstdint>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

using namespace ns3;

/**
 * @ingroup tests
 *
 * Verify application observations use configured windows and peer-local IAT streams.
 */
class ExperimentAppTestCase : public TestCase
{
  public:
    ExperimentAppTestCase();

  private:
    void DoRun() override;
};

ExperimentAppTestCase::ExperimentAppTestCase()
    : TestCase("collect windowed application and sink statistics")
{
}

void
ExperimentAppTestCase::DoRun()
{
    TrafficCoordinator coordinator(25.0, 25.0);
    WifiStatistics statistics(coordinator, 10);

    Ptr<Node> accessPointNode = CreateObject<Node>();
    Ptr<APGenerator> generator = CreateObject<APGenerator>();
    generator->SetReadyCallback(coordinator.GetReadyCallback());
    accessPointNode->AddApplication(generator);
    generator->SetStartTime(Seconds(0));
    coordinator.AddGenerator(generator);
    coordinator.AddApplication(generator);
    coordinator.FinalizeRegistration();
    Simulator::Run();

    const int64_t epochUs = coordinator.GetExperimentStartUs();
    NS_TEST_ASSERT_MSG_EQ(epochUs, 1000000, "Wrong test experiment epoch");

    const uint32_t apNodeId = accessPointNode->GetId();
    const uint32_t firstStationNodeId = apNodeId + 10;
    const uint32_t secondStationNodeId = apNodeId + 20;
    statistics.RegisterAccessPointIdentity(0, apNodeId, "AP0", Ipv4Address("10.1.0.1"));
    statistics.RegisterStationIdentity(0,
                                       0,
                                       firstStationNodeId,
                                       "AP0/STA0",
                                       Ipv4Address("10.1.0.2"));
    statistics.RegisterStationIdentity(0,
                                       1,
                                       secondStationNodeId,
                                       "AP0/STA1",
                                       Ipv4Address("10.1.0.3"));
    statistics.ConnectApGenerator(generator, apNodeId);
    statistics.ConnectStaGenerator(CreateObject<StaLlmGenerator>(), firstStationNodeId);
    statistics.ConnectTrafficSink(CreateObject<TrafficSink>(),
                                  apNodeId,
                                  ExperimentDirection::UPLINK);

    statistics.RecordAcceptedApplicationSend(apNodeId,
                                             ExperimentDirection::DOWNLINK,
                                             secondStationNodeId,
                                             "agent-z",
                                             83,
                                             epochUs + 1000);
    statistics.RecordAcceptedApplicationSend(apNodeId,
                                             ExperimentDirection::DOWNLINK,
                                             firstStationNodeId,
                                             "agent-a",
                                             61,
                                             epochUs + 9999);
    statistics.RecordApplicationDrop(apNodeId,
                                     ExperimentDirection::DOWNLINK,
                                     firstStationNodeId,
                                     "agent-a",
                                     7,
                                     epochUs + 9999);
    statistics.RecordAcceptedApplicationSend(firstStationNodeId,
                                             ExperimentDirection::UPLINK,
                                             apNodeId,
                                             "agent-u",
                                             47,
                                             epochUs + 10000);
    statistics.RecordApplicationDrop(firstStationNodeId,
                                     ExperimentDirection::UPLINK,
                                     apNodeId,
                                     "agent-u",
                                     5,
                                     epochUs + 10000);

    statistics.RecordApplicationReceive(apNodeId,
                                        ExperimentDirection::UPLINK,
                                        firstStationNodeId,
                                        31,
                                        epochUs + 9000);
    statistics.RecordApplicationReceive(apNodeId,
                                        ExperimentDirection::UPLINK,
                                        secondStationNodeId,
                                        41,
                                        epochUs + 10000);
    statistics.RecordApplicationReceive(apNodeId,
                                        ExperimentDirection::UPLINK,
                                        firstStationNodeId,
                                        53,
                                        epochUs + 12000);
    statistics.RecordApplicationReceive(apNodeId,
                                        ExperimentDirection::UPLINK,
                                        std::nullopt,
                                        17,
                                        epochUs + 14000);
    statistics.RecordApplicationReceive(apNodeId,
                                        ExperimentDirection::UPLINK,
                                        secondStationNodeId,
                                        67,
                                        epochUs + 16000);
    statistics.RecordApplicationReceive(firstStationNodeId,
                                        ExperimentDirection::DOWNLINK,
                                        apNodeId,
                                        71,
                                        epochUs + 20000);

    statistics.RecordAcceptedApplicationSend(apNodeId,
                                             ExperimentDirection::DOWNLINK,
                                             firstStationNodeId,
                                             "outside",
                                             999,
                                             epochUs + 25000);
    statistics.RecordApplicationReceive(apNodeId,
                                        ExperimentDirection::UPLINK,
                                        firstStationNodeId,
                                        999,
                                        epochUs - 1);

    const auto& windows = statistics.m_state->unifiedWindows;
    NS_TEST_ASSERT_MSG_EQ(windows.size(), 3, "Out-of-duration APP event created a window");

    const auto& firstWindowDownlink = windows.at(0).at(apNodeId).app.downlink;
    NS_TEST_ASSERT_MSG_EQ(firstWindowDownlink.acceptedSendCount,
                          2,
                          "Wrong AP downlink accepted-send count");
    NS_TEST_ASSERT_MSG_EQ(firstWindowDownlink.acceptedPayloadBytes,
                          144,
                          "AP downlink did not use actual accepted bytes");
    NS_TEST_ASSERT_MSG_EQ(firstWindowDownlink.dropEventCount, 1, "Wrong AP downlink drop count");
    NS_TEST_ASSERT_MSG_EQ(firstWindowDownlink.droppedPayloadBytes,
                          7,
                          "Wrong AP downlink drop bytes");
    NS_TEST_ASSERT_MSG_EQ(firstWindowDownlink.agents.size(), 2, "Wrong AP agent map size");
    NS_TEST_ASSERT_MSG_EQ(firstWindowDownlink.agents.begin()->first,
                          "agent-a",
                          "Agent map is not deterministic");
    NS_TEST_ASSERT_MSG_EQ(firstWindowDownlink.agents.at("agent-a").acceptedPayloadBytes,
                          61,
                          "Wrong first agent accepted bytes");
    NS_TEST_ASSERT_MSG_EQ(firstWindowDownlink.agents.at("agent-a").droppedPayloadBytes,
                          7,
                          "Wrong first agent dropped bytes");
    NS_TEST_ASSERT_MSG_EQ(firstWindowDownlink.peersByNodeId.size(), 2, "Wrong AP peer map size");
    NS_TEST_ASSERT_MSG_EQ(firstWindowDownlink.peersByNodeId.begin()->first,
                          firstStationNodeId,
                          "Peer map is not deterministic");
    NS_TEST_ASSERT_MSG_EQ(
        firstWindowDownlink.peersByNodeId.at(secondStationNodeId).acceptedPayloadBytes,
        83,
        "Wrong second peer accepted bytes");
    const auto& firstWindowApUplink = windows.at(0).at(apNodeId).app.uplink;
    NS_TEST_ASSERT_MSG_EQ(firstWindowApUplink.receiveEventCount,
                          1,
                          "Wrong first-window AP sink receive count");
    NS_TEST_ASSERT_MSG_EQ(firstWindowApUplink.receivedPayloadBytes,
                          31,
                          "Wrong first-window AP sink received bytes");

    const auto& secondWindowStaUplink = windows.at(1).at(firstStationNodeId).app.uplink;
    NS_TEST_ASSERT_MSG_EQ(secondWindowStaUplink.acceptedSendCount,
                          1,
                          "Wrong STA uplink accepted-send count");
    NS_TEST_ASSERT_MSG_EQ(secondWindowStaUplink.acceptedPayloadBytes,
                          47,
                          "Wrong STA uplink accepted bytes");
    NS_TEST_ASSERT_MSG_EQ(secondWindowStaUplink.peersByNodeId.at(apNodeId).droppedPayloadBytes,
                          5,
                          "Wrong STA peer drop bytes");

    const auto& secondWindowApUplink = windows.at(1).at(apNodeId).app.uplink;
    NS_TEST_ASSERT_MSG_EQ(secondWindowApUplink.receiveEventCount, 4, "Wrong AP sink receive count");
    NS_TEST_ASSERT_MSG_EQ(secondWindowApUplink.receivedPayloadBytes,
                          178,
                          "Wrong AP sink received bytes");
    NS_TEST_ASSERT_MSG_EQ(secondWindowApUplink.peersByNodeId.size(),
                          2,
                          "Unknown sink peer created a peer entry");
    NS_TEST_ASSERT_MSG_EQ(secondWindowApUplink.receiveInterArrivalUs.count,
                          2,
                          "Cross-peer receives polluted IAT streams");
    NS_TEST_ASSERT_MSG_EQ(secondWindowApUplink.receiveInterArrivalUs.sum,
                          9000.0,
                          "Wrong merged peer-local IAT sum");
    NS_TEST_ASSERT_MSG_EQ(secondWindowApUplink.receiveInterArrivalUs.minimum,
                          3000.0,
                          "Wrong minimum peer-local IAT");
    NS_TEST_ASSERT_MSG_EQ(secondWindowApUplink.receiveInterArrivalUs.maximum,
                          6000.0,
                          "Wrong maximum peer-local IAT");

    const auto& finalWindowStaDownlink = windows.at(2).at(firstStationNodeId).app.downlink;
    NS_TEST_ASSERT_MSG_EQ(finalWindowStaDownlink.receiveEventCount,
                          1,
                          "Wrong STA sink receive count");
    NS_TEST_ASSERT_MSG_EQ(finalWindowStaDownlink.receivedPayloadBytes,
                          71,
                          "Wrong STA sink received bytes");
    NS_TEST_ASSERT_MSG_EQ(windows.contains(3), false, "Event at duration boundary was included");

    Simulator::Destroy();
}

std::vector<TestCase*>
CreateExperimentAppTestCases()
{
    return {new ExperimentAppTestCase};
}
