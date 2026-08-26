#include "../../examples/runtime/traffic-coordinator.h"
#include "../../examples/statistics/experiment-statistics.h"
#include "../../examples/statistics/internal.h"
#include "../llm-test-suite.h"

#include "ns3/ap-generator.h"
#include "ns3/network-module.h"
#include "ns3/sta-llm-generator.h"
#include "ns3/traffic-sink.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

using namespace ns3;

namespace
{

/** One AP accepted-send or drop trace event. */
struct ApApplicationTraceEvent
{
    Address station;      ///< Station trace identity.
    std::string agentKey; ///< Agent trace identity.
    uint32_t bytes;       ///< Accepted or dropped bytes.
    Time time;            ///< Trace event time.
};

/** One STA accepted-send or drop trace event. */
struct StaApplicationTraceEvent
{
    std::string agentKey; ///< Agent trace identity.
    uint32_t bytes;       ///< Accepted or dropped bytes.
    Time time;            ///< Trace event time.
};

/** Capture one AP application trace event. */
void
CaptureApApplicationTrace(std::vector<ApApplicationTraceEvent>* events,
                          Address station,
                          std::string agentKey,
                          uint32_t bytes,
                          Time time)
{
    events->push_back({station, std::move(agentKey), bytes, time});
}

/** Capture one STA application trace event. */
void
CaptureStaApplicationTrace(std::vector<StaApplicationTraceEvent>* events,
                           std::string agentKey,
                           uint32_t bytes,
                           Time time)
{
    events->push_back({std::move(agentKey), bytes, time});
}

/**
 * @ingroup tests
 *
 * Verify every application trace advertises its exact declared callback type.
 */
class ApplicationTraceMetadataTestCase : public TestCase
{
  public:
    ApplicationTraceMetadataTestCase();

  private:
    void DoRun() override;
    void ExpectCallback(TypeId typeId,
                        const std::string& traceName,
                        const std::string& expectedCallback);
};

ApplicationTraceMetadataTestCase::ApplicationTraceMetadataTestCase()
    : TestCase("advertise exact application trace callback metadata")
{
}

void
ApplicationTraceMetadataTestCase::ExpectCallback(TypeId typeId,
                                                 const std::string& traceName,
                                                 const std::string& expectedCallback)
{
    TypeId::TraceSourceInformation information;
    NS_TEST_ASSERT_MSG_NE(typeId.LookupTraceSourceByName(traceName, &information),
                          nullptr,
                          "Missing trace " << typeId.GetName() << "::" << traceName);
    NS_TEST_ASSERT_MSG_EQ(information.callback,
                          expectedCallback,
                          "Wrong callback metadata for " << typeId.GetName() << "::" << traceName);
}

void
ApplicationTraceMetadataTestCase::DoRun()
{
    ExpectCallback(APGenerator::GetTypeId(), "Tx", "ns3::APGenerator::AcceptedSendCallback");
    ExpectCallback(APGenerator::GetTypeId(), "AppTxDrop", "ns3::APGenerator::DropCallback");
    ExpectCallback(APGenerator::GetTypeId(), "AgentSend", "ns3::APGenerator::AgentSendCallback");
    ExpectCallback(StaLlmGenerator::GetTypeId(),
                   "TxCustom",
                   "ns3::StaLlmGenerator::AcceptedSendCallback");
    ExpectCallback(StaLlmGenerator::GetTypeId(), "AppTxDrop", "ns3::StaLlmGenerator::DropCallback");
    ExpectCallback(StaLlmGenerator::GetTypeId(),
                   "AgentSend",
                   "ns3::StaLlmGenerator::AgentSendCallback");
    ExpectCallback(TrafficSink::GetTypeId(), "RxCustom", "ns3::TrafficSink::RxCallback");
}

} // namespace

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
    ExperimentStatistics statistics(coordinator, 10);

    Ptr<Node> accessPointNode = CreateObject<Node>();
    Ptr<APGenerator> generator = CreateObject<APGenerator>();
    generator->SetReadyCallback(coordinator.GetReadyCallback());
    accessPointNode->AddApplication(generator);
    generator->SetStartTime(Seconds(0));
    coordinator.AddGenerator(generator);
    coordinator.AddApplication(generator);
    coordinator.FinalizeRegistration();
    Simulator::Stop(MicroSeconds(1));
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
    Ptr<StaLlmGenerator> stationGenerator = CreateObject<StaLlmGenerator>();
    Ptr<TrafficSink> accessPointSink = CreateObject<TrafficSink>();
    Ptr<TrafficSink> stationSink = CreateObject<TrafficSink>();
    statistics.ConnectApGenerator(generator, apNodeId);
    statistics.ConnectStaGenerator(stationGenerator, firstStationNodeId);
    statistics.ConnectTrafficSink(accessPointSink, apNodeId, ExperimentDirection::UPLINK);
    statistics.ConnectTrafficSink(stationSink, firstStationNodeId, ExperimentDirection::DOWNLINK);

    std::vector<ApApplicationTraceEvent> apAcceptedEvents;
    std::vector<ApApplicationTraceEvent> apDropEvents;
    std::vector<StaApplicationTraceEvent> staAcceptedEvents;
    std::vector<StaApplicationTraceEvent> staDropEvents;
    NS_TEST_ASSERT_MSG_EQ(generator->TraceConnectWithoutContext(
                              "Tx",
                              MakeBoundCallback(&CaptureApApplicationTrace, &apAcceptedEvents)),
                          true,
                          "Could not capture AP accepted-send trace");
    NS_TEST_ASSERT_MSG_EQ(generator->TraceConnectWithoutContext(
                              "AppTxDrop",
                              MakeBoundCallback(&CaptureApApplicationTrace, &apDropEvents)),
                          true,
                          "Could not capture AP drop trace");
    NS_TEST_ASSERT_MSG_EQ(stationGenerator->TraceConnectWithoutContext(
                              "TxCustom",
                              MakeBoundCallback(&CaptureStaApplicationTrace, &staAcceptedEvents)),
                          true,
                          "Could not capture STA accepted-send trace");
    NS_TEST_ASSERT_MSG_EQ(stationGenerator->TraceConnectWithoutContext(
                              "AppTxDrop",
                              MakeBoundCallback(&CaptureStaApplicationTrace, &staDropEvents)),
                          true,
                          "Could not capture STA drop trace");

    const Address firstStationAddress = InetSocketAddress(Ipv4Address("10.1.0.2"), 5001);
    const Address secondStationAddress = InetSocketAddress(Ipv4Address("10.1.0.3"), 5002);
    const Address accessPointAddress = InetSocketAddress(Ipv4Address("10.1.0.1"), 5000);

    NS_TEST_ASSERT_MSG_EQ(generator->EmitSendResult(secondStationAddress,
                                                    "agent-z",
                                                    83,
                                                    83,
                                                    MicroSeconds(epochUs + 1000)),
                          true,
                          "AP full send was rejected");
    NS_TEST_ASSERT_MSG_EQ(generator->EmitSendResult(firstStationAddress,
                                                    "agent-a",
                                                    68,
                                                    61,
                                                    MicroSeconds(epochUs + 9999)),
                          true,
                          "AP partial send was rejected");
    NS_TEST_ASSERT_MSG_EQ(generator->EmitSendResult(firstStationAddress,
                                                    "agent-f",
                                                    13,
                                                    -1,
                                                    MicroSeconds(epochUs + 5000)),
                          false,
                          "AP failed send was accepted");
    NS_TEST_ASSERT_MSG_EQ(generator->EmitSendResult(firstStationAddress,
                                                    "outside",
                                                    999,
                                                    999,
                                                    MicroSeconds(epochUs + 25000)),
                          true,
                          "Out-of-duration AP send was rejected by the generator seam");

    NS_TEST_ASSERT_MSG_EQ(
        stationGenerator->EmitSendResult("agent-u", 47, 47, MicroSeconds(epochUs + 10000)),
        true,
        "STA full send was rejected");
    NS_TEST_ASSERT_MSG_EQ(
        stationGenerator->EmitSendResult("agent-v", 50, 43, MicroSeconds(epochUs + 11000)),
        true,
        "STA partial send was rejected");
    NS_TEST_ASSERT_MSG_EQ(
        stationGenerator->EmitSendResult("agent-w", 9, -1, MicroSeconds(epochUs + 12000)),
        false,
        "STA failed send was accepted");

    accessPointSink->EmitReceive(999, Address());
    const int64_t nowUs = Simulator::Now().GetMicroSeconds();
    Simulator::Schedule(MicroSeconds(epochUs + 9000 - nowUs),
                        &TrafficSink::EmitReceive,
                        accessPointSink,
                        31,
                        firstStationAddress);
    Simulator::Schedule(MicroSeconds(epochUs + 10000 - nowUs),
                        &TrafficSink::EmitReceive,
                        accessPointSink,
                        41,
                        secondStationAddress);
    Simulator::Schedule(MicroSeconds(epochUs + 12000 - nowUs),
                        &TrafficSink::EmitReceive,
                        accessPointSink,
                        53,
                        firstStationAddress);
    Simulator::Schedule(MicroSeconds(epochUs + 14000 - nowUs),
                        &TrafficSink::EmitReceive,
                        accessPointSink,
                        17,
                        Address());
    Simulator::Schedule(MicroSeconds(epochUs + 15000 - nowUs),
                        &TrafficSink::EmitReceive,
                        accessPointSink,
                        19,
                        Address());
    Simulator::Schedule(MicroSeconds(epochUs + 16000 - nowUs),
                        &TrafficSink::EmitReceive,
                        accessPointSink,
                        67,
                        secondStationAddress);
    Simulator::Schedule(MicroSeconds(epochUs + 20000 - nowUs),
                        &TrafficSink::EmitReceive,
                        stationSink,
                        71,
                        accessPointAddress);
    Simulator::Run();

    const auto& windows = statistics.m_state->unifiedWindows;
    NS_TEST_ASSERT_MSG_EQ(windows.size(), 3, "Out-of-duration APP event created a window");

    const auto& firstWindowDownlink = windows.at(0).at(apNodeId).app.downlink;
    NS_TEST_ASSERT_MSG_EQ(firstWindowDownlink.acceptedSendCount,
                          2,
                          "Wrong AP downlink accepted-send count");
    NS_TEST_ASSERT_MSG_EQ(firstWindowDownlink.acceptedPayloadBytes,
                          144,
                          "AP downlink did not use actual accepted bytes");
    NS_TEST_ASSERT_MSG_EQ(firstWindowDownlink.dropEventCount, 2, "Wrong AP downlink drop count");
    NS_TEST_ASSERT_MSG_EQ(firstWindowDownlink.droppedPayloadBytes,
                          20,
                          "Wrong AP downlink drop bytes");
    NS_TEST_ASSERT_MSG_EQ(firstWindowDownlink.agents.size(), 3, "Wrong AP agent map size");
    NS_TEST_ASSERT_MSG_EQ(firstWindowDownlink.agents.begin()->first,
                          "agent-a",
                          "Agent map is not deterministic");
    NS_TEST_ASSERT_MSG_EQ(firstWindowDownlink.agents.at("agent-a").acceptedPayloadBytes,
                          61,
                          "Wrong first agent accepted bytes");
    NS_TEST_ASSERT_MSG_EQ(firstWindowDownlink.agents.at("agent-a").droppedPayloadBytes,
                          7,
                          "Wrong first agent dropped bytes");
    NS_TEST_ASSERT_MSG_EQ(firstWindowDownlink.agents.at("agent-f").droppedPayloadBytes,
                          13,
                          "Wrong failed AP send bytes");
    NS_TEST_ASSERT_MSG_EQ(firstWindowDownlink.peersByNodeId.size(), 2, "Wrong AP peer map size");
    NS_TEST_ASSERT_MSG_EQ(firstWindowDownlink.peersByNodeId.begin()->first,
                          firstStationNodeId,
                          "Peer map is not deterministic");
    NS_TEST_ASSERT_MSG_EQ(
        firstWindowDownlink.peersByNodeId.at(firstStationNodeId).droppedPayloadBytes,
        20,
        "Wrong first AP peer drop bytes");
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
                          2,
                          "Wrong STA uplink accepted-send count");
    NS_TEST_ASSERT_MSG_EQ(secondWindowStaUplink.acceptedPayloadBytes,
                          90,
                          "Wrong STA uplink accepted bytes");
    NS_TEST_ASSERT_MSG_EQ(secondWindowStaUplink.dropEventCount, 2, "Wrong STA uplink drop count");
    NS_TEST_ASSERT_MSG_EQ(secondWindowStaUplink.droppedPayloadBytes,
                          16,
                          "Wrong STA uplink drop bytes");
    NS_TEST_ASSERT_MSG_EQ(secondWindowStaUplink.peersByNodeId.at(apNodeId).droppedPayloadBytes,
                          16,
                          "Wrong STA peer drop bytes");
    NS_TEST_ASSERT_MSG_EQ(secondWindowStaUplink.agents.at("agent-v").acceptedPayloadBytes,
                          43,
                          "STA trace reported requested rather than accepted bytes");
    NS_TEST_ASSERT_MSG_EQ(secondWindowStaUplink.agents.at("agent-w").droppedPayloadBytes,
                          9,
                          "Wrong failed STA send bytes");

    const auto& secondWindowApUplink = windows.at(1).at(apNodeId).app.uplink;
    NS_TEST_ASSERT_MSG_EQ(secondWindowApUplink.receiveEventCount, 5, "Wrong AP sink receive count");
    NS_TEST_ASSERT_MSG_EQ(secondWindowApUplink.receivedPayloadBytes,
                          197,
                          "Wrong AP sink received bytes");
    NS_TEST_ASSERT_MSG_EQ(secondWindowApUplink.peersByNodeId.size(),
                          2,
                          "Unknown sink peer created a peer entry");
    NS_TEST_ASSERT_MSG_EQ(
        secondWindowApUplink.peersByNodeId.at(firstStationNodeId).receivedPayloadBytes,
        53,
        "Known first sink peer was not resolved");
    NS_TEST_ASSERT_MSG_EQ(
        secondWindowApUplink.peersByNodeId.at(secondStationNodeId).receivedPayloadBytes,
        108,
        "Known second sink peer was not resolved");
    NS_TEST_ASSERT_MSG_EQ(secondWindowApUplink.receiveInterArrivalUs.count,
                          3,
                          "Cross-peer receives polluted IAT streams");
    NS_TEST_ASSERT_MSG_EQ(secondWindowApUplink.receiveInterArrivalUs.sum,
                          10000.0,
                          "Wrong merged peer-local IAT sum");
    NS_TEST_ASSERT_MSG_EQ(secondWindowApUplink.receiveInterArrivalUs.minimum,
                          1000.0,
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
    NS_TEST_ASSERT_MSG_EQ(finalWindowStaDownlink.peersByNodeId.at(apNodeId).receivedPayloadBytes,
                          71,
                          "STA sink did not resolve its AP peer");
    NS_TEST_ASSERT_MSG_EQ(windows.contains(3), false, "Event at duration boundary was included");

    NS_TEST_ASSERT_MSG_EQ(apAcceptedEvents.size(), 3, "Wrong AP accepted trace count");
    NS_TEST_ASSERT_MSG_EQ(apAcceptedEvents.at(0).station,
                          secondStationAddress,
                          "AP accepted trace lost station identity");
    NS_TEST_ASSERT_MSG_EQ(apAcceptedEvents.at(1).agentKey,
                          "agent-a",
                          "AP accepted trace lost agent identity");
    NS_TEST_ASSERT_MSG_EQ(apAcceptedEvents.at(1).station,
                          firstStationAddress,
                          "AP partial-send trace lost station identity");
    NS_TEST_ASSERT_MSG_EQ(apAcceptedEvents.at(1).bytes,
                          61,
                          "AP accepted trace reported requested bytes");
    NS_TEST_ASSERT_MSG_EQ(apAcceptedEvents.at(1).time.GetMicroSeconds(),
                          epochUs + 9999,
                          "AP accepted trace changed transmit time");
    NS_TEST_ASSERT_MSG_EQ(apDropEvents.size(), 2, "Wrong AP drop trace count");
    NS_TEST_ASSERT_MSG_EQ(apDropEvents.at(0).bytes, 7, "Wrong AP partial-send remainder");
    NS_TEST_ASSERT_MSG_EQ(apDropEvents.at(0).agentKey,
                          "agent-a",
                          "AP drop trace lost agent identity");
    NS_TEST_ASSERT_MSG_EQ(apDropEvents.at(0).station,
                          firstStationAddress,
                          "AP drop trace lost station identity");
    NS_TEST_ASSERT_MSG_EQ(apDropEvents.at(1).bytes, 13, "Wrong AP failed-send drop bytes");
    NS_TEST_ASSERT_MSG_EQ(staAcceptedEvents.size(), 2, "Wrong STA accepted trace count");
    NS_TEST_ASSERT_MSG_EQ(staAcceptedEvents.at(1).agentKey,
                          "agent-v",
                          "STA accepted trace lost agent identity");
    NS_TEST_ASSERT_MSG_EQ(staAcceptedEvents.at(1).bytes,
                          43,
                          "STA accepted trace reported requested bytes");
    NS_TEST_ASSERT_MSG_EQ(staAcceptedEvents.at(1).time.GetMicroSeconds(),
                          epochUs + 11000,
                          "STA accepted trace changed transmit time");
    NS_TEST_ASSERT_MSG_EQ(staDropEvents.size(), 2, "Wrong STA drop trace count");
    NS_TEST_ASSERT_MSG_EQ(staDropEvents.at(0).bytes, 7, "Wrong STA partial-send remainder");
    NS_TEST_ASSERT_MSG_EQ(staDropEvents.at(1).bytes, 9, "Wrong STA failed-send drop bytes");

    Simulator::Destroy();
}

std::vector<TestCase*>
CreateExperimentAppTestCases()
{
    return {new ApplicationTraceMetadataTestCase, new ExperimentAppTestCase};
}
