#include "../examples/traffic-coordinator.h"
#include "../examples/wifi-statistics-internal.h"
#include "llm-test-suite.h"

#include "ns3/ap-generator.h"
#include "ns3/arp-header.h"
#include "ns3/iana-ieee802-numbers.h"
#include "ns3/iana-internet-protocol-numbers.h"
#include "ns3/ipv4-header.h"
#include "ns3/llc-snap-header.h"
#include "ns3/mac48-address.h"
#include "ns3/network-module.h"
#include "ns3/tcp-header.h"
#include "ns3/udp-header.h"

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
 * Build one IPv4 Wi-Fi device payload with an explicit transport protocol.
 *
 * @param ipProtocol IPv4 next-header protocol number.
 * @return Packet containing LLC/SNAP, IPv4, transport, and payload bytes.
 */
Ptr<Packet>
BuildIpv4DevicePacket(uint8_t ipProtocol)
{
    Ptr<Packet> packet;
    if (ipProtocol == iana::internetprotocolnumbers::TCP)
    {
        packet = Create<Packet>(64);
        TcpHeader tcp;
        tcp.SetSourcePort(9000);
        tcp.SetDestinationPort(10000);
        packet->AddHeader(tcp);
    }
    else
    {
        uint8_t payload[32]{};
        payload[4] = 0x50;
        packet = Create<Packet>(payload, sizeof(payload));
        UdpHeader udp;
        udp.SetSourcePort(9000);
        udp.SetDestinationPort(10000);
        packet->AddHeader(udp);
    }

    Ipv4Header ipv4;
    ipv4.SetSource(Ipv4Address("10.1.0.2"));
    ipv4.SetDestination(Ipv4Address("10.1.0.1"));
    ipv4.SetProtocol(ipProtocol);
    ipv4.SetPayloadSize(packet->GetSize());
    packet->AddHeader(ipv4);

    LlcSnapHeader llc;
    llc.SetType(iana::ieee802numbers::IPV4);
    packet->AddHeader(llc);
    return packet;
}

/**
 * Build one padded ARP request with valid hardware and protocol endpoints.
 *
 * @return Packet containing LLC/SNAP and a serialized ARP request.
 */
Ptr<Packet>
BuildArpDevicePacket()
{
    Ptr<Packet> packet = Create<Packet>(32);
    ArpHeader arp;
    arp.SetRequest(Mac48Address("00:00:00:00:00:02"),
                   Ipv4Address("10.1.0.2"),
                   Mac48Address::GetBroadcast(),
                   Ipv4Address("10.1.0.1"));
    packet->AddHeader(arp);

    LlcSnapHeader llc;
    llc.SetType(iana::ieee802numbers::ARP);
    packet->AddHeader(llc);
    return packet;
}

/**
 * @ingroup tests
 *
 * Verify the device parser rejects non-IPv4 LLC payloads and non-TCP IPv4 payloads.
 */
class DevicePacketProtocolValidationTestCase : public TestCase
{
  public:
    DevicePacketProtocolValidationTestCase();

  private:
    void DoRun() override;
};

DevicePacketProtocolValidationTestCase::DevicePacketProtocolValidationTestCase()
    : TestCase("reject non-IPv4 and non-TCP device payloads")
{
}

void
DevicePacketProtocolValidationTestCase::DoRun()
{
    TrafficCoordinator coordinator(30.0, 30.0);
    const int64_t epochUs = OpenExperiment(coordinator);

    WifiStatisticsState nonIpv4Statistics(coordinator, 10);
    RegisterEntities(nonIpv4Statistics);
    const Ptr<Packet> nonIpv4Packet = BuildArpDevicePacket();
    NS_TEST_ASSERT_MSG_EQ(
        RecordDeviceTransmitPacket(nonIpv4Statistics, epochUs + 1000, nonIpv4Packet),
        false,
        "Non-IPv4 LLC payload was accepted");
    NS_TEST_ASSERT_MSG_EQ(nonIpv4Statistics.unifiedWindows.empty(),
                          true,
                          "Non-IPv4 LLC payload created window state");
    NS_TEST_ASSERT_MSG_EQ(nonIpv4Statistics.deviceTransmitsByFlow.empty(),
                          true,
                          "Non-IPv4 LLC payload created matching state");

    WifiStatisticsState udpStatistics(coordinator, 10);
    RegisterEntities(udpStatistics);
    const Ptr<Packet> udpPacket = BuildIpv4DevicePacket(iana::internetprotocolnumbers::UDP);
    NS_TEST_ASSERT_MSG_EQ(RecordDeviceTransmitPacket(udpStatistics, epochUs + 1000, udpPacket),
                          false,
                          "IPv4/UDP payload was accepted as TCP");
    NS_TEST_ASSERT_MSG_EQ(udpStatistics.unifiedWindows.empty(),
                          true,
                          "IPv4/UDP payload created window state");
    NS_TEST_ASSERT_MSG_EQ(udpStatistics.deviceTransmitsByFlow.empty(),
                          true,
                          "IPv4/UDP payload created matching state");

    WifiStatisticsState tcpStatistics(coordinator, 10);
    RegisterEntities(tcpStatistics);
    const Ptr<Packet> tcpPacket = BuildIpv4DevicePacket(iana::internetprotocolnumbers::TCP);
    NS_TEST_ASSERT_MSG_EQ(RecordDeviceTransmitPacket(tcpStatistics, epochUs + 1000, tcpPacket),
                          true,
                          "Valid IPv4/TCP payload was rejected");
    NS_TEST_ASSERT_MSG_EQ(
        tcpStatistics.unifiedWindows.at(0).at(20).mac.uplink.estimatedTransmitEventCount,
        1,
        "Valid IPv4/TCP payload did not reach device statistics");

    Simulator::Destroy();
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
    RecordMacMpduDrop(statistics, 10, epochUs + 2000, 9, 700, 20);
    RecordMacMpduDrop(statistics, 10, epochUs + 3000, 4, 300, 20);
    RecordMacMpduDrop(statistics, 10, epochUs + 3500, 7, 100);
    RecordMacDataFailure(statistics, 10, epochUs + 4000, false, 20);
    RecordMacDataFailure(statistics, 10, epochUs + 4500, false);
    RecordMacDataFailure(statistics, 10, epochUs + 5000, true, 20);

    RecordMacTransmitDrop(statistics, 20, epochUs + 10000, 600);
    RecordMacMpduDrop(statistics, 20, epochUs + 11000, 5, 200, 10);
    RecordMacDataFailure(statistics, 20, epochUs + 12000, false, 10);
    RecordMacDataFailure(statistics, 20, epochUs + 13000, true, 10);
    RecordMacTransmitDrop(statistics, 20, epochUs + 30000, 999);

    const auto& accessPoint = statistics.unifiedWindows.at(0).at(10).mac.downlink;
    NS_TEST_ASSERT_MSG_EQ(accessPoint.transmitDropCount, 1, "Wrong AP transmit-drop count");
    NS_TEST_ASSERT_MSG_EQ(accessPoint.transmitDropPacketBytes,
                          500,
                          "Wrong AP transmit-drop packet bytes");
    NS_TEST_ASSERT_MSG_EQ(accessPoint.mpduDropCount, 3, "Wrong AP MPDU-drop count");
    NS_TEST_ASSERT_MSG_EQ(accessPoint.mpduDropBytes, 1100, "Wrong AP MPDU-drop bytes");
    NS_TEST_ASSERT_MSG_EQ(accessPoint.dataFailureCount, 2, "Wrong AP data-failure count");
    NS_TEST_ASSERT_MSG_EQ(accessPoint.finalDataFailureCount,
                          1,
                          "Wrong AP final-data-failure count");
    NS_TEST_ASSERT_MSG_EQ(accessPoint.mpduDropsByReason.size(), 3, "Wrong reason count");
    NS_TEST_ASSERT_MSG_EQ(accessPoint.mpduDropsByReason.begin()->first,
                          4,
                          "MAC reasons are not numerically ordered");
    NS_TEST_ASSERT_MSG_EQ(accessPoint.mpduDropsByReason.at(4), 1, "Wrong reason 4 count");
    NS_TEST_ASSERT_MSG_EQ(accessPoint.mpduDropsByReason.at(7), 1, "Wrong reason 7 count");
    NS_TEST_ASSERT_MSG_EQ(accessPoint.mpduDropsByReason.at(9), 1, "Wrong reason 9 count");
    NS_TEST_ASSERT_MSG_EQ(accessPoint.peersByNodeId.size(),
                          1,
                          "Unresolved AP MAC events created a peer");
    const auto& accessPointPeer = accessPoint.peersByNodeId.at(20);
    NS_TEST_ASSERT_MSG_EQ(accessPointPeer.mpduDropCount, 2, "Wrong AP peer MPDU-drop count");
    NS_TEST_ASSERT_MSG_EQ(accessPointPeer.mpduDropBytes, 1000, "Wrong AP peer MPDU-drop bytes");
    NS_TEST_ASSERT_MSG_EQ(accessPointPeer.mpduDropsByReason.size(),
                          2,
                          "Wrong AP peer reason count");
    NS_TEST_ASSERT_MSG_EQ(accessPointPeer.mpduDropsByReason.begin()->first,
                          4,
                          "AP peer reasons are not numerically ordered");
    NS_TEST_ASSERT_MSG_EQ(accessPointPeer.mpduDropsByReason.at(4),
                          1,
                          "Wrong AP peer reason 4 count");
    NS_TEST_ASSERT_MSG_EQ(accessPointPeer.mpduDropsByReason.at(9),
                          1,
                          "Wrong AP peer reason 9 count");
    NS_TEST_ASSERT_MSG_EQ(accessPointPeer.dataFailureCount, 1, "Wrong AP peer data-failure count");
    NS_TEST_ASSERT_MSG_EQ(accessPointPeer.finalDataFailureCount,
                          1,
                          "Wrong AP peer final-data-failure count");
    NS_TEST_ASSERT_MSG_EQ(statistics.unifiedWindows.at(0).at(10).mac.uplink.transmitDropCount,
                          0,
                          "AP transmitter event mapped to uplink");

    const auto& station = statistics.unifiedWindows.at(1).at(20).mac.uplink;
    NS_TEST_ASSERT_MSG_EQ(station.transmitDropCount, 1, "Wrong STA transmit-drop count");
    NS_TEST_ASSERT_MSG_EQ(station.transmitDropPacketBytes, 600, "Wrong STA transmit-drop bytes");
    NS_TEST_ASSERT_MSG_EQ(station.mpduDropCount, 1, "Wrong STA MPDU-drop count");
    NS_TEST_ASSERT_MSG_EQ(station.mpduDropBytes, 200, "Wrong STA MPDU-drop bytes");
    NS_TEST_ASSERT_MSG_EQ(station.dataFailureCount, 1, "Wrong STA data-failure count");
    NS_TEST_ASSERT_MSG_EQ(station.finalDataFailureCount, 1, "Wrong STA final-data-failure count");
    NS_TEST_ASSERT_MSG_EQ(station.peersByNodeId.size(), 1, "Wrong STA MAC peer count");
    const auto& stationPeer = station.peersByNodeId.at(10);
    NS_TEST_ASSERT_MSG_EQ(stationPeer.mpduDropCount, 1, "Wrong STA peer MPDU-drop count");
    NS_TEST_ASSERT_MSG_EQ(stationPeer.mpduDropBytes, 200, "Wrong STA peer MPDU-drop bytes");
    NS_TEST_ASSERT_MSG_EQ(stationPeer.mpduDropsByReason.at(5),
                          1,
                          "Wrong STA peer MPDU-drop reason");
    NS_TEST_ASSERT_MSG_EQ(stationPeer.dataFailureCount, 1, "Wrong STA peer data-failure count");
    NS_TEST_ASSERT_MSG_EQ(stationPeer.finalDataFailureCount,
                          1,
                          "Wrong STA peer final-data-failure count");
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
                          1100,
                          "Legacy cross-layer MPDU bytes were not retained");
    NS_TEST_ASSERT_MSG_EQ(legacyAccessPoint.macDataFailures,
                          2,
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
    return {new DevicePacketProtocolValidationTestCase,
            new ExperimentDeviceMatchingTestCase,
            new ExperimentMacEventTestCase};
}
