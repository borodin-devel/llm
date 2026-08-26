#include "../../examples/runtime/traffic-coordinator.h"
#include "../../examples/statistics/internal.h"
#include "../llm-test-suite.h"

#include "ns3/ap-generator.h"
#include "ns3/app-tx-tag.h"
#include "ns3/network-module.h"
#include "ns3/wifi-module.h"

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
 * Register one test BSS in exact lookup state.
 *
 * @param statistics Test statistics state.
 */
void
RegisterExactEntities(ExperimentStatisticsState& statistics)
{
    statistics.entityRegistry.RegisterAccessPoint(0, 10, "AP0", "10.1.0.1");
    statistics.entityRegistry.RegisterStation(0, 0, 20, "AP0/STA0", "10.1.0.2");
    statistics.entityRegistry.RegisterStation(0, 1, 21, "AP0/STA1", "10.1.0.3");
}

/** Register one test BSS in exact lookup state. @param statistics Test state. */
void
RegisterEntities(ExperimentStatisticsState& statistics)
{
    RegisterExactEntities(statistics);
}

/**
 * Build one application-tagged payload.
 *
 * @param sourceIpv4 Tagged source IPv4 address.
 * @param destinationIpv4 Tagged destination IPv4 address.
 * @param applicationPacketUid Tagged application packet identifier.
 * @param applicationTransmitTimeUs Tagged application transmit time in microseconds.
 * @param bytes Payload size in bytes.
 * @return Tagged payload packet.
 */
Ptr<Packet>
BuildTaggedPayload(const char* sourceIpv4,
                   const char* destinationIpv4,
                   uint64_t applicationPacketUid,
                   int64_t applicationTransmitTimeUs,
                   uint32_t bytes)
{
    Ptr<Packet> packet = Create<Packet>(bytes);
    packet->AddByteTag(AppTxTag(applicationPacketUid,
                                applicationTransmitTimeUs,
                                Ipv4Address(sourceIpv4),
                                Ipv4Address(destinationIpv4),
                                9000,
                                10000,
                                bytes,
                                "test-agent"));
    return packet;
}

/**
 * Build one data header with literal MPDU identity fields.
 *
 * @param receiver Receiver MAC address.
 * @param transmitter Transmitter MAC address.
 * @param sequenceNumber MPDU sequence number.
 * @return Wi-Fi data header.
 */
WifiMacHeader
BuildDataHeader(const char* receiver, const char* transmitter, uint16_t sequenceNumber)
{
    WifiMacHeader header;
    header.SetType(WIFI_MAC_DATA);
    header.SetAddr1(Mac48Address(receiver));
    header.SetAddr2(Mac48Address(transmitter));
    header.SetAddr3(Mac48Address(transmitter));
    header.SetSequenceNumber(sequenceNumber);
    header.SetFragmentNumber(0);
    return header;
}

/**
 * Build a two-user HE MU transmission vector with distinct user rates.
 *
 * @return Configured HE MU transmission vector.
 */
WifiTxVector
BuildTwoUserTxVector()
{
    WifiTxVector txVector(HePhy::GetHeMcs0(),
                          WIFI_MIN_TX_PWR_LEVEL,
                          WIFI_PREAMBLE_HE_MU,
                          NanoSeconds(800),
                          1,
                          1,
                          0,
                          MHz_u{20},
                          false);
    txVector.SetHeMuUserInfo(1, {HeRu::RuSpec{RuType::RU_106_TONE, 1, true}, 0, 1});
    txVector.SetHeMuUserInfo(2, {HeRu::RuSpec{RuType::RU_106_TONE, 2, true}, 7, 1});
    txVector.SetSigBMode(VhtPhy::GetVhtMcs0());
    return txVector;
}

/** @ingroup tests Verify tagged MPDU identity, direction, peer, and delay attribution. */
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
    ExperimentStatisticsState statistics(coordinator, 10);
    RegisterEntities(statistics);

    const std::vector<PhyTaggedPayloadSpan> uplinkSpans{
        {"10.1.0.2", "10.1.0.1", epochUs + 9000, 100},
        {"10.1.0.2", "10.1.0.1", epochUs + 9500, 50},
    };
    const PhyMpduKey uplinkIdentity{
        20,
        "00:00:00:00:00:01",
        "00:00:00:00:00:02",
        7,
        0,
        99,
    };
    RecordPhyMpduAttempt(statistics, 20, epochUs + 11000, 500, uplinkSpans, uplinkIdentity);
    RecordPhyMpduAttempt(statistics, 20, epochUs + 12000, 500, uplinkSpans, uplinkIdentity);

    const auto& station = statistics.unifiedWindows.at(1).at(20);
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

    const auto& accessPoint = statistics.unifiedWindows.at(1).at(10);
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
    NS_TEST_ASSERT_MSG_EQ(statistics.unifiedWindows.contains(0),
                          false,
                          "Delay was assigned to the application timestamp window");

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

    Simulator::Destroy();
}

/** @ingroup tests Verify PPDU rate weighting and peer attribution. */
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
    ExperimentStatisticsState statistics(coordinator, 10);
    RegisterEntities(statistics);

    RecordPhyRateAttempt(statistics, 20, epochUs + 2000, "10.1.0.2", "10.1.0.1", 10e6, 100.0L);
    RecordPhyRateAttempt(statistics, 20, epochUs + 3000, "10.1.0.2", "10.1.0.1", 20e6, 300.0L);

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
    const PhyPeerAccumulator noAirtime;
    NS_TEST_ASSERT_MSG_EQ(CalculateAveragePhyDataRateMbps(noAirtime).has_value(),
                          false,
                          "No airtime must produce a null average later");

    Simulator::Destroy();
}

/** @ingroup tests Verify callback-shared tag and MPDU identity parsing. */
class ExperimentPhyPacketParsingTestCase : public TestCase
{
  public:
    ExperimentPhyPacketParsingTestCase();

  private:
    void DoRun() override;
};

ExperimentPhyPacketParsingTestCase::ExperimentPhyPacketParsingTestCase()
    : TestCase("parse a real tagged Wi-Fi MPDU through the PHY callback path")
{
}

void
ExperimentPhyPacketParsingTestCase::DoRun()
{
    TrafficCoordinator coordinator(30.0, 30.0);
    const int64_t epochUs = OpenExperiment(coordinator);
    ExperimentStatisticsState statistics(coordinator, 10);
    RegisterEntities(statistics);

    Ptr<Packet> packet = BuildTaggedPayload("10.1.0.2", "10.1.0.1", 77, epochUs + 9000, 120);
    packet->AddHeader(BuildDataHeader("00:00:00:00:00:01", "00:00:00:00:00:02", 42));
    RecordPhyTxBeginPacket(statistics, 20, epochUs + 11000, packet);
    RecordPhyTxBeginPacket(statistics, 20, epochUs + 12000, packet);

    const auto& uplink = statistics.unifiedWindows.at(1).at(20).phy.uplink;
    NS_TEST_ASSERT_MSG_EQ(uplink.taggedPayloadBytes, 240, "Tagged packet bytes were not parsed");
    NS_TEST_ASSERT_MSG_EQ(uplink.uniqueTaggedPayloadBytes,
                          120,
                          "Parsed MPDU identity did not suppress retry uniqueness");
    NS_TEST_ASSERT_MSG_EQ(uplink.retransmissionCount,
                          1,
                          "Parsed repeated MPDU identity was not a retry");
    NS_TEST_ASSERT_MSG_EQ(uplink.completeTaggedMpduBytes,
                          2 * packet->GetSize(),
                          "Parsed complete MPDU size is wrong");
    NS_TEST_ASSERT_MSG_EQ(
        statistics.unifiedWindows.at(1).at(20).applicationToPhyDelayUs.uplink.count,
        1,
        "Parsed first MPDU did not produce exactly one delay sample");
    NS_TEST_ASSERT_MSG_EQ(statistics.unifiedWindows.contains(0),
                          false,
                          "Parsed delay used the application timestamp window");

    Simulator::Destroy();
}

/** @ingroup tests Verify per-user rate parsing and proportional airtime. */
class ExperimentPhyPsduParsingTestCase : public TestCase
{
  public:
    ExperimentPhyPsduParsingTestCase();

  private:
    void DoRun() override;
};

ExperimentPhyPsduParsingTestCase::ExperimentPhyPsduParsingTestCase()
    : TestCase("parse per-user rates and allocate tagged PPDU airtime")
{
}

void
ExperimentPhyPsduParsingTestCase::DoRun()
{
    TrafficCoordinator coordinator(30.0, 30.0);
    const int64_t epochUs = OpenExperiment(coordinator);
    const WifiTxVector txVector = BuildTwoUserTxVector();
    Ptr<Packet> firstUserPayload =
        BuildTaggedPayload("10.1.0.1", "10.1.0.2", 80, epochUs + 1000, 40);
    firstUserPayload->AddAtEnd(BuildTaggedPayload("10.1.0.1", "10.1.0.2", 82, epochUs + 1000, 60));
    const WifiConstPsduMap psduMap{
        {1,
         Create<WifiPsdu>(firstUserPayload,
                          BuildDataHeader("00:00:00:00:00:02", "00:00:00:00:00:01", 50))},
        {2,
         Create<WifiPsdu>(BuildTaggedPayload("10.1.0.1", "10.1.0.3", 81, epochUs + 1000, 300),
                          BuildDataHeader("00:00:00:00:00:03", "00:00:00:00:00:01", 51))},
    };

    ExperimentStatisticsState wrongTransmitter(coordinator, 10);
    RegisterExactEntities(wrongTransmitter);
    RecordPhyTxPsduBegin(wrongTransmitter,
                         20,
                         epochUs + 2000,
                         WIFI_PHY_BAND_5GHZ,
                         psduMap,
                         txVector);
    NS_TEST_ASSERT_MSG_EQ(wrongTransmitter.unifiedWindows.empty(),
                          true,
                          "PSDU flow was attributed to a node other than the callback transmitter");

    ExperimentStatisticsState statistics(coordinator, 10);
    RegisterEntities(statistics);
    RecordPhyTxPsduBegin(statistics, 10, epochUs + 2000, WIFI_PHY_BAND_5GHZ, psduMap, txVector);

    const long double ppduAirtimeUs =
        static_cast<long double>(
            WifiPhy::CalculateTxDuration(psduMap, txVector, WIFI_PHY_BAND_5GHZ).GetNanoSeconds()) /
        1000.0L;
    const auto& downlink = statistics.unifiedWindows.at(0).at(10).phy.downlink;
    const auto& firstPeer = downlink.peersByNodeId.at(20);
    const auto& secondPeer = downlink.peersByNodeId.at(21);
    NS_TEST_ASSERT_MSG_EQ(downlink.transmissionAttemptCount, 2, "Wrong grouped flow attempts");
    NS_TEST_ASSERT_MSG_EQ_TOL(static_cast<double>(firstPeer.transmissionAirtimeUs),
                              static_cast<double>(ppduAirtimeUs * 0.25L),
                              1e-9,
                              "First user's proportional airtime is wrong");
    NS_TEST_ASSERT_MSG_EQ_TOL(static_cast<double>(secondPeer.transmissionAirtimeUs),
                              static_cast<double>(ppduAirtimeUs * 0.75L),
                              1e-9,
                              "Second user's proportional airtime is wrong");
    NS_TEST_ASSERT_MSG_EQ_TOL(*CalculateAveragePhyDataRateMbps(firstPeer),
                              static_cast<double>(txVector.GetMode(1).GetDataRate(txVector, 1)) /
                                  1e6,
                              1e-12,
                              "First user's PHY rate was not extracted by STA ID");
    NS_TEST_ASSERT_MSG_EQ_TOL(*CalculateAveragePhyDataRateMbps(secondPeer),
                              static_cast<double>(txVector.GetMode(2).GetDataRate(txVector, 2)) /
                                  1e6,
                              1e-12,
                              "Second user's PHY rate was not extracted by STA ID");

    Simulator::Destroy();
}

/** @ingroup tests Verify local busy time splitting at configured boundaries. */
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
    ExperimentStatisticsState statistics(coordinator, 10);
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
    Simulator::Destroy();
}

} // namespace

std::vector<TestCase*>
CreateExperimentPhyTestCases()
{
    return {new ExperimentPhyMpduAttributionTestCase,
            new ExperimentPhyRateTestCase,
            new ExperimentPhyPacketParsingTestCase,
            new ExperimentPhyPsduParsingTestCase,
            new ExperimentPhyBusyTimeTestCase};
}
