#include "../../examples/runtime/traffic-coordinator.h"
#include "../../examples/statistics/experiment-statistics.h"
#include "../../examples/statistics/internal.h"
#include "../llm-test-suite.h"

#include "ns3/ap-generator.h"
#include "ns3/network-module.h"
#include "ns3/sta-llm-generator.h"

#include <cstdint>
#include <string>
#include <vector>

using namespace ns3;

namespace
{

/** One generator TCP trace event. */
struct TcpTraceEvent
{
    Address peer;  ///< Remote socket address.
    int64_t value; ///< CWND bytes or RTT microseconds.
    Time time;     ///< Trace event time.
};

/**
 * Capture one congestion-window trace event.
 *
 * @param events Destination event list.
 * @param peer Remote socket address.
 * @param congestionWindowBytes New congestion window in bytes.
 * @param eventTime Trace event time.
 */
void
CaptureCongestionWindow(std::vector<TcpTraceEvent>* events,
                        Address peer,
                        uint32_t congestionWindowBytes,
                        Time eventTime)
{
    events->push_back({peer, congestionWindowBytes, eventTime});
}

/**
 * Capture one round-trip-time trace event.
 *
 * @param events Destination event list.
 * @param peer Remote socket address.
 * @param roundTripTime Round-trip-time sample.
 * @param eventTime Trace event time.
 */
void
CaptureRoundTripTime(std::vector<TcpTraceEvent>* events,
                     Address peer,
                     Time roundTripTime,
                     Time eventTime)
{
    events->push_back({peer, roundTripTime.GetMicroSeconds(), eventTime});
}

} // namespace

/**
 * @ingroup tests
 *
 * Verify generator TCP traces preserve peer identity, values, and event time.
 */
class GeneratorTcpTraceTestCase : public TestCase
{
  public:
    GeneratorTcpTraceTestCase();

  private:
    void DoRun() override;

    /**
     * Verify one trace source advertises its exact callback type.
     *
     * @param typeId Generator type identifier.
     * @param traceName Trace source name.
     * @param expectedCallback Expected callback metadata.
     */
    void ExpectCallback(TypeId typeId,
                        const std::string& traceName,
                        const std::string& expectedCallback);
};

GeneratorTcpTraceTestCase::GeneratorTcpTraceTestCase()
    : TestCase("emit exact per-peer TCP transport traces")
{
}

void
GeneratorTcpTraceTestCase::ExpectCallback(TypeId typeId,
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
GeneratorTcpTraceTestCase::DoRun()
{
    ExpectCallback(APGenerator::GetTypeId(),
                   "CongestionWindowSample",
                   "ns3::APGenerator::CongestionWindowSampleCallback");
    ExpectCallback(APGenerator::GetTypeId(),
                   "RoundTripTimeSample",
                   "ns3::APGenerator::RoundTripTimeSampleCallback");
    ExpectCallback(StaLlmGenerator::GetTypeId(),
                   "CongestionWindowSample",
                   "ns3::StaLlmGenerator::CongestionWindowSampleCallback");
    ExpectCallback(StaLlmGenerator::GetTypeId(),
                   "RoundTripTimeSample",
                   "ns3::StaLlmGenerator::RoundTripTimeSampleCallback");

    const Address apPeer = InetSocketAddress(Ipv4Address("10.1.0.2"), 5001);
    const Address staPeer = InetSocketAddress(Ipv4Address("10.1.0.1"), 5000);
    Ptr<APGenerator> apGenerator = CreateObject<APGenerator>();
    Ptr<StaLlmGenerator> staGenerator = CreateObject<StaLlmGenerator>();
    std::vector<TcpTraceEvent> apCongestionWindows;
    std::vector<TcpTraceEvent> apRoundTripTimes;
    std::vector<TcpTraceEvent> staCongestionWindows;
    std::vector<TcpTraceEvent> staRoundTripTimes;

    NS_TEST_ASSERT_MSG_EQ(apGenerator->TraceConnectWithoutContext(
                              "CongestionWindowSample",
                              MakeBoundCallback(&CaptureCongestionWindow, &apCongestionWindows)),
                          true,
                          "Could not capture AP CWND trace");
    NS_TEST_ASSERT_MSG_EQ(apGenerator->TraceConnectWithoutContext(
                              "RoundTripTimeSample",
                              MakeBoundCallback(&CaptureRoundTripTime, &apRoundTripTimes)),
                          true,
                          "Could not capture AP RTT trace");
    NS_TEST_ASSERT_MSG_EQ(staGenerator->TraceConnectWithoutContext(
                              "CongestionWindowSample",
                              MakeBoundCallback(&CaptureCongestionWindow, &staCongestionWindows)),
                          true,
                          "Could not capture STA CWND trace");
    NS_TEST_ASSERT_MSG_EQ(staGenerator->TraceConnectWithoutContext(
                              "RoundTripTimeSample",
                              MakeBoundCallback(&CaptureRoundTripTime, &staRoundTripTimes)),
                          true,
                          "Could not capture STA RTT trace");

    staGenerator->SetRemote(staPeer);
    Simulator::Schedule(MicroSeconds(111), [apGenerator, apPeer]() {
        apGenerator->RecordCongestionWindow(apPeer, 1000, 2000);
    });
    Simulator::Schedule(MicroSeconds(112), [apGenerator, apPeer]() {
        apGenerator->RecordRoundTripTime(apPeer, MicroSeconds(10), MicroSeconds(125));
    });
    Simulator::Schedule(MicroSeconds(113), [apGenerator, apPeer]() {
        apGenerator->RecordRoundTripTime(apPeer, MicroSeconds(125), MicroSeconds(0));
    });
    Simulator::Schedule(MicroSeconds(211),
                        [staGenerator]() { staGenerator->RecordCongestionWindow(2000, 3000); });
    Simulator::Schedule(MicroSeconds(212), [staGenerator]() {
        staGenerator->RecordRoundTripTime(MicroSeconds(20), MicroSeconds(250));
    });
    Simulator::Run();

    NS_TEST_ASSERT_MSG_EQ(apCongestionWindows.size(), 1, "Wrong AP CWND trace count");
    NS_TEST_ASSERT_MSG_EQ(apCongestionWindows.at(0).peer, apPeer, "AP CWND lost peer identity");
    NS_TEST_ASSERT_MSG_EQ(apCongestionWindows.at(0).value, 2000, "AP CWND lost new value");
    NS_TEST_ASSERT_MSG_EQ(apCongestionWindows.at(0).time.GetMicroSeconds(),
                          111,
                          "AP CWND lost event time");
    NS_TEST_ASSERT_MSG_EQ(apRoundTripTimes.size(), 1, "AP emitted a zero RTT sample");
    NS_TEST_ASSERT_MSG_EQ(apRoundTripTimes.at(0).value, 125, "AP RTT lost new value");
    NS_TEST_ASSERT_MSG_EQ(staCongestionWindows.size(), 1, "Wrong STA CWND trace count");
    NS_TEST_ASSERT_MSG_EQ(staCongestionWindows.at(0).peer,
                          staPeer,
                          "STA CWND did not use configured remote");
    NS_TEST_ASSERT_MSG_EQ(staRoundTripTimes.at(0).peer,
                          staPeer,
                          "STA RTT did not use configured remote");
    NS_TEST_ASSERT_MSG_EQ(staRoundTripTimes.at(0).value, 250, "STA RTT lost new value");
    NS_TEST_ASSERT_MSG_EQ(staRoundTripTimes.at(0).time.GetMicroSeconds(),
                          212,
                          "STA RTT lost event time");

    Simulator::Destroy();
}

/**
 * @ingroup tests
 *
 * Verify CWND is time-weighted by peer and RTT remains a raw event-window sample.
 */
class ExperimentTcpTestCase : public TestCase
{
  public:
    ExperimentTcpTestCase();

  private:
    void DoRun() override;
};

ExperimentTcpTestCase::ExperimentTcpTestCase()
    : TestCase("collect per-peer TCP window statistics")
{
}

void
ExperimentTcpTestCase::DoRun()
{
    TrafficCoordinator coordinator(30.0, 30.0);
    ExperimentStatistics statistics(coordinator, 10);
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

    const int64_t epochUs = coordinator.GetExperimentStartUs();
    const uint32_t ownerNodeId = node->GetId();
    const uint32_t firstPeerNodeId = ownerNodeId + 10;
    const uint32_t secondPeerNodeId = ownerNodeId + 20;

    statistics.RecordCongestionWindow(ownerNodeId,
                                      ExperimentDirection::DOWNLINK,
                                      firstPeerNodeId,
                                      1000,
                                      epochUs - 5000);
    statistics.RecordCongestionWindow(ownerNodeId,
                                      ExperimentDirection::DOWNLINK,
                                      secondPeerNodeId,
                                      8000,
                                      epochUs - 2000);
    statistics.RecordCongestionWindow(ownerNodeId,
                                      ExperimentDirection::DOWNLINK,
                                      firstPeerNodeId,
                                      2000,
                                      epochUs + 5000);
    statistics.RecordCongestionWindow(ownerNodeId,
                                      ExperimentDirection::DOWNLINK,
                                      firstPeerNodeId,
                                      4000,
                                      epochUs + 15000);

    statistics.RecordRoundTripTime(ownerNodeId,
                                   ExperimentDirection::DOWNLINK,
                                   firstPeerNodeId,
                                   100,
                                   epochUs + 2000);
    statistics.RecordRoundTripTime(ownerNodeId,
                                   ExperimentDirection::DOWNLINK,
                                   firstPeerNodeId,
                                   300,
                                   epochUs + 8000);
    statistics.RecordRoundTripTime(ownerNodeId,
                                   ExperimentDirection::DOWNLINK,
                                   firstPeerNodeId,
                                   200,
                                   epochUs + 12000);
    statistics.RecordRoundTripTime(ownerNodeId,
                                   ExperimentDirection::DOWNLINK,
                                   secondPeerNodeId,
                                   700,
                                   epochUs + 7000);

    statistics.FinalizeTcpStatistics();
    statistics.FinalizeTcpStatistics();

    const auto& windows = statistics.m_state->unifiedWindows;
    const auto key = std::make_pair(ExperimentDirection::DOWNLINK, firstPeerNodeId);
    const auto secondKey = std::make_pair(ExperimentDirection::DOWNLINK, secondPeerNodeId);
    const auto& firstWindow = windows.at(0).at(ownerNodeId).tcpConnections.at(key);
    const auto& secondWindow = windows.at(1).at(ownerNodeId).tcpConnections.at(key);
    const auto& thirdWindow = windows.at(2).at(ownerNodeId).tcpConnections.at(key);

    NS_TEST_ASSERT_MSG_EQ_TOL(firstWindow.congestionWindowBytesUs / 10000.0L,
                              1500.0L,
                              1e-9L,
                              "Wrong first-window time-weighted CWND");
    NS_TEST_ASSERT_MSG_EQ(firstWindow.congestionWindowObservationDurationUs,
                          10000,
                          "Wrong first-window CWND duration");
    NS_TEST_ASSERT_MSG_EQ_TOL(secondWindow.congestionWindowBytesUs / 10000.0L,
                              3000.0L,
                              1e-9L,
                              "Wrong second-window time-weighted CWND");
    NS_TEST_ASSERT_MSG_EQ_TOL(thirdWindow.congestionWindowBytesUs / 10000.0L,
                              4000.0L,
                              1e-9L,
                              "Wrong final-window time-weighted CWND");
    const long double overallCwnd =
        (firstWindow.congestionWindowBytesUs + secondWindow.congestionWindowBytesUs +
         thirdWindow.congestionWindowBytesUs) /
        30000.0L;
    NS_TEST_ASSERT_MSG_EQ_TOL(overallCwnd,
                              85000.0L / 30.0L,
                              1e-9L,
                              "Wrong overall time-weighted CWND");
    NS_TEST_ASSERT_MSG_EQ(firstWindow.lastCongestionWindowBytes.value(),
                          2000,
                          "Wrong first-window terminal CWND");
    NS_TEST_ASSERT_MSG_EQ(secondWindow.lastCongestionWindowBytes.value(),
                          4000,
                          "Wrong second-window terminal CWND");
    NS_TEST_ASSERT_MSG_EQ(thirdWindow.lastCongestionWindowBytes.value(),
                          4000,
                          "Wrong final-window terminal CWND");

    const auto& independentPeer = windows.at(0).at(ownerNodeId).tcpConnections.at(secondKey);
    NS_TEST_ASSERT_MSG_EQ(independentPeer.congestionWindowObservationDurationUs,
                          10000,
                          "Second peer did not retain independent duration");
    NS_TEST_ASSERT_MSG_EQ_TOL(independentPeer.congestionWindowBytesUs / 10000.0L,
                              8000.0L,
                              1e-9L,
                              "Independent peer CWND was combined");

    NS_TEST_ASSERT_MSG_EQ(firstWindow.roundTripTimeUs.count, 2, "Wrong RTT sample count");
    NS_TEST_ASSERT_MSG_EQ(firstWindow.roundTripTimeUs.sum, 400.0L, "Wrong RTT sample sum");
    NS_TEST_ASSERT_MSG_EQ(firstWindow.roundTripTimeUs.sumSquares,
                          100000.0L,
                          "Wrong RTT sample square sum");
    NS_TEST_ASSERT_MSG_EQ(firstWindow.roundTripTimeUs.minimum, 100.0, "Wrong minimum RTT");
    NS_TEST_ASSERT_MSG_EQ(firstWindow.roundTripTimeUs.maximum, 300.0, "Wrong maximum RTT");
    NS_TEST_ASSERT_MSG_EQ(secondWindow.roundTripTimeUs.count,
                          1,
                          "RTT sample landed in the wrong event window");
    NS_TEST_ASSERT_MSG_EQ(secondWindow.roundTripTimeUs.sum, 200.0L, "Wrong second-window RTT");
    NS_TEST_ASSERT_MSG_EQ(independentPeer.roundTripTimeUs.count,
                          1,
                          "RTT samples from independent peers were combined");
    NS_TEST_ASSERT_MSG_EQ(independentPeer.roundTripTimeUs.sum,
                          700.0L,
                          "Wrong independent peer RTT sum");

    Simulator::Destroy();
}

std::vector<TestCase*>
CreateExperimentTcpTestCases()
{
    return {new GeneratorTcpTraceTestCase, new ExperimentTcpTestCase};
}
