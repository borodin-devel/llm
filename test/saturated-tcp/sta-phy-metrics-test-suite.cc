#include "../../examples/saturated-tcp/sta-phy-metrics.h"
#include "../llm-test-suite.h"

#include "ns3/he-phy.h"
#include "ns3/ofdm-phy.h"
#include "ns3/packet.h"
#include "ns3/test.h"
#include "ns3/wifi-mac-header.h"
#include "ns3/wifi-mpdu.h"
#include "ns3/wifi-ns3-constants.h"
#include "ns3/wifi-phy.h"
#include "ns3/wifi-psdu.h"
#include "ns3/wifi-tx-vector.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace ns3;

namespace
{

/** Registered station address used by the PPDU fixture. */
const Mac48Address STATION_ADDRESS("00:00:00:00:00:02");

/** Unicast access-point address used by the PPDU fixture. */
const Mac48Address ACCESS_POINT_ADDRESS("00:00:00:00:00:01");

/** Unrelated transmitter address used by the PPDU fixture. */
const Mac48Address UNRELATED_ADDRESS("00:00:00:00:00:03");

/**
 * Build a valid legacy OFDM transmission vector.
 *
 * @param mode Selected nominal mode.
 * @return Configured single-user transmission vector.
 */
WifiTxVector
BuildLegacyTxVector(WifiMode mode = OfdmPhy::GetOfdmRate54Mbps())
{
    return WifiTxVector(mode,
                        WIFI_MIN_TX_PWR_LEVEL,
                        WIFI_PREAMBLE_LONG,
                        NanoSeconds(800),
                        1,
                        1,
                        0,
                        MHz_u{20},
                        false);
}

/**
 * Build a valid two-user HE MU transmission vector with equal rates.
 *
 * @return Configured multi-user transmission vector.
 */
WifiTxVector
BuildEqualRateTwoUserTxVector()
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
    txVector.SetHeMuUserInfo(2, {HeRu::RuSpec{RuType::RU_106_TONE, 2, true}, 0, 1});
    txVector.SetSigBMode(VhtPhy::GetVhtMcs0());
    return txVector;
}

/**
 * Build a valid HE SU transmission vector for an A-MPDU.
 *
 * @return Configured aggregate transmission vector.
 */
WifiTxVector
BuildAggregateTxVector()
{
    return WifiTxVector(HePhy::GetHeMcs3(),
                        WIFI_MIN_TX_PWR_LEVEL,
                        WIFI_PREAMBLE_HE_SU,
                        NanoSeconds(800),
                        1,
                        1,
                        0,
                        MHz_u{20},
                        true);
}

/**
 * Build one PSDU fixture with literal MAC identities.
 *
 * @param type MAC frame type.
 * @param payloadBytes Bytes following the MAC header.
 * @param receiver Receiver address.
 * @param transmitter Transmitter address where the type carries one.
 * @param retry Whether to set the retry bit.
 * @return Constructed PSDU.
 */
Ptr<WifiPsdu>
BuildPsdu(WifiMacType type,
          uint32_t payloadBytes,
          Mac48Address receiver = ACCESS_POINT_ADDRESS,
          Mac48Address transmitter = STATION_ADDRESS,
          bool retry = false)
{
    WifiMacHeader header(type);
    header.SetAddr1(receiver);
    header.SetAddr2(transmitter);
    header.SetAddr3(ACCESS_POINT_ADDRESS);
    if (retry)
    {
        header.SetRetry();
    }
    return Create<WifiPsdu>(Create<Packet>(payloadBytes), header);
}

/**
 * Build an A-MPDU containing two qualifying station data MPDUs.
 *
 * @return Constructed aggregate PSDU.
 */
Ptr<WifiPsdu>
BuildAggregatePsdu()
{
    WifiMacHeader firstHeader(WIFI_MAC_QOSDATA);
    firstHeader.SetAddr1(ACCESS_POINT_ADDRESS);
    firstHeader.SetAddr2(STATION_ADDRESS);
    firstHeader.SetAddr3(ACCESS_POINT_ADDRESS);
    firstHeader.SetQosTid(0);
    firstHeader.SetSequenceNumber(1);
    WifiMacHeader secondHeader = firstHeader;
    secondHeader.SetSequenceNumber(2);
    std::vector<Ptr<WifiMpdu>> mpdus{
        Create<WifiMpdu>(Create<Packet>(300), firstHeader),
        Create<WifiMpdu>(Create<Packet>(500), secondHeader),
    };
    return Create<WifiPsdu>(std::move(mpdus));
}

/**
 * Build an A-MPDU with one qualifying data MPDU and one null-data MPDU.
 *
 * @return Constructed partially qualifying aggregate PSDU.
 */
Ptr<WifiPsdu>
BuildPartiallyQualifyingAggregatePsdu()
{
    WifiMacHeader dataHeader(WIFI_MAC_QOSDATA);
    dataHeader.SetAddr1(ACCESS_POINT_ADDRESS);
    dataHeader.SetAddr2(STATION_ADDRESS);
    dataHeader.SetAddr3(ACCESS_POINT_ADDRESS);
    dataHeader.SetQosTid(0);
    dataHeader.SetSequenceNumber(1);
    WifiMacHeader nullHeader(WIFI_MAC_QOSDATA_NULL);
    nullHeader.SetAddr1(ACCESS_POINT_ADDRESS);
    nullHeader.SetAddr2(STATION_ADDRESS);
    nullHeader.SetAddr3(ACCESS_POINT_ADDRESS);
    nullHeader.SetQosTid(0);
    nullHeader.SetSequenceNumber(2);
    std::vector<Ptr<WifiMpdu>> mpdus{
        Create<WifiMpdu>(Create<Packet>(300), dataHeader),
        Create<WifiMpdu>(Create<Packet>(500), nullHeader),
    };
    return Create<WifiPsdu>(std::move(mpdus));
}

/**
 * Record one single-user PPDU fixture.
 *
 * @param recorder Metric recorder.
 * @param stationId Callback station identifier.
 * @param startNs PPDU start in nanoseconds.
 * @param psdu Transmitted PSDU.
 * @param txVector Actual transmission vector.
 */
void
RecordPsdu(StationPhyMetricRecorder& recorder,
           uint32_t stationId,
           int64_t startNs,
           Ptr<const WifiPsdu> psdu,
           const WifiTxVector& txVector = BuildLegacyTxVector())
{
    recorder.RecordPpduAttempt(stationId,
                               startNs,
                               WIFI_PHY_BAND_5GHZ,
                               {{SU_STA_ID, psdu}},
                               txVector);
}

/** @ingroup tests Verify literal station PHY metric formulas and validation. */
class StationPhyMetricFormulaTestCase : public TestCase
{
  public:
    StationPhyMetricFormulaTestCase();

  private:
    void DoRun() override;
};

StationPhyMetricFormulaTestCase::StationPhyMetricFormulaTestCase()
    : TestCase("derive station PHY rates, efficiency, and contention from raw values")
{
}

void
StationPhyMetricFormulaTestCase::DoRun()
{
    StationPhyMetricAccumulator raw;
    raw.nominalRateBpsNs = 100'000'000.0L * 100'000.0L + 200'000'000.0L * 300'000.0L;
    raw.psduBits = static_cast<long double>((1000 + 6000) * 8);
    raw.ppduAirtimeNs = 400'000;
    raw.contentionNs = 100'000;

    const auto output = DeriveStationPhyMetrics(raw, 1'000'000);
    NS_TEST_ASSERT_MSG_EQ(output.averageTheoreticalPhyRateMbps.has_value(),
                          true,
                          "Positive PPDU airtime did not produce a theoretical rate");
    NS_TEST_ASSERT_MSG_EQ(output.averagePracticalPhyRateMbps.has_value(),
                          true,
                          "Positive PPDU airtime did not produce a practical rate");
    NS_TEST_ASSERT_MSG_EQ(output.channelEfficiency.has_value(),
                          true,
                          "Positive theoretical rate did not produce efficiency");
    NS_TEST_ASSERT_MSG_EQ(output.contentionFraction.has_value(),
                          true,
                          "Positive window duration did not produce contention");
    NS_TEST_ASSERT_MSG_EQ_TOL(*output.averageTheoreticalPhyRateMbps,
                              175.0,
                              1e-12,
                              "Wrong airtime-weighted theoretical PHY rate");
    NS_TEST_ASSERT_MSG_EQ_TOL(*output.averagePracticalPhyRateMbps,
                              140.0,
                              1e-12,
                              "Wrong PSDU-bit practical PHY rate");
    NS_TEST_ASSERT_MSG_EQ_TOL(*output.channelEfficiency, 0.8, 1e-12, "Wrong channel efficiency");
    NS_TEST_ASSERT_MSG_EQ_TOL(*output.contentionFraction, 0.1, 1e-12, "Wrong contention fraction");

    StationPhyMetricAccumulator idle;
    idle.contentionNs = 100'000;
    const auto idleOutput = DeriveStationPhyMetrics(idle, 1'000'000);
    NS_TEST_ASSERT_MSG_EQ(idleOutput.averageTheoreticalPhyRateMbps.has_value(),
                          false,
                          "Zero PPDU airtime produced a theoretical rate");
    NS_TEST_ASSERT_MSG_EQ(idleOutput.averagePracticalPhyRateMbps.has_value(),
                          false,
                          "Zero PPDU airtime produced a practical rate");
    NS_TEST_ASSERT_MSG_EQ(idleOutput.channelEfficiency.has_value(),
                          false,
                          "Zero PPDU airtime produced channel efficiency");
    NS_TEST_ASSERT_MSG_EQ_TOL(*idleOutput.contentionFraction,
                              0.1,
                              1e-12,
                              "Valid idle-window contention was lost");

    const auto zeroWindowOutput = DeriveStationPhyMetrics({}, 0);
    NS_TEST_ASSERT_MSG_EQ(zeroWindowOutput.contentionFraction.has_value(),
                          false,
                          "Zero contention denominator did not produce null");

    StationPhyMetricAccumulator impossible;
    impossible.nominalRateBpsNs = 100'000'000.0L * 100'000.0L;
    impossible.psduBits = 20'000.0L;
    impossible.ppduAirtimeNs = 100'000;
    bool rejected = false;
    try
    {
        static_cast<void>(DeriveStationPhyMetrics(impossible, 1'000'000));
    }
    catch (const std::invalid_argument&)
    {
        rejected = true;
    }
    NS_TEST_ASSERT_MSG_EQ(rejected,
                          true,
                          "Practical PHY rate materially above theoretical was accepted");
}

/** @ingroup tests Verify unit-range clamping and malformed raw-value rejection. */
class StationPhyMetricRangeTestCase : public TestCase
{
  public:
    StationPhyMetricRangeTestCase();

  private:
    void DoRun() override;
};

StationPhyMetricRangeTestCase::StationPhyMetricRangeTestCase()
    : TestCase("validate and clamp station PHY metric ranges")
{
}

void
StationPhyMetricRangeTestCase::DoRun()
{
    StationPhyMetricAccumulator boundary;
    boundary.nominalRateBpsNs = 1'000'000.0L * 1'000'000.0L;
    boundary.psduBits = 1'000.0L * (1.0L + 5e-13L);
    boundary.ppduAirtimeNs = 1'000'000;
    boundary.contentionNs = 1'000'000'000'001;
    const auto output = DeriveStationPhyMetrics(boundary, 1'000'000'000'000);
    NS_TEST_ASSERT_MSG_EQ(*output.channelEfficiency,
                          1.0,
                          "Efficiency within boundary tolerance was not clamped");
    NS_TEST_ASSERT_MSG_EQ(*output.contentionFraction,
                          1.0,
                          "Contention within boundary tolerance was not clamped");

    std::array<StationPhyMetricAccumulator, 5> malformed{};
    malformed[0].nominalRateBpsNs = -1.0L;
    malformed[1].psduBits = -1.0L;
    malformed[2].ppduAirtimeNs = -1;
    malformed[3].nominalRateBpsNs = std::numeric_limits<long double>::infinity();
    malformed[4].nominalRateBpsNs = std::numeric_limits<long double>::max();
    malformed[4].ppduAirtimeNs = 1;
    for (const auto& raw : malformed)
    {
        bool rejected = false;
        try
        {
            static_cast<void>(DeriveStationPhyMetrics(raw, 1'000'000));
        }
        catch (const std::invalid_argument&)
        {
            rejected = true;
        }
        NS_TEST_ASSERT_MSG_EQ(rejected, true, "Malformed raw station metric was accepted");
    }

    StationPhyMetricAccumulator excessiveContention;
    excessiveContention.contentionNs = 1'000'001;
    bool rejected = false;
    try
    {
        static_cast<void>(DeriveStationPhyMetrics(excessiveContention, 1'000'000));
    }
    catch (const std::invalid_argument&)
    {
        rejected = true;
    }
    NS_TEST_ASSERT_MSG_EQ(rejected, true, "Material contention overflow was accepted");
}

/** @ingroup tests Verify station registration and complete PPDU frame classification. */
class StationPhyMetricClassificationTestCase : public TestCase
{
  public:
    StationPhyMetricClassificationTestCase();

  private:
    void DoRun() override;
};

StationPhyMetricClassificationTestCase::StationPhyMetricClassificationTestCase()
    : TestCase("classify only qualifying registered-station PPDU attempts")
{
}

void
StationPhyMetricClassificationTestCase::DoRun()
{
    StationPhyMetricRecorder recorder(0, 100'000'000, 10'000'000);
    recorder.RegisterStation(7, STATION_ADDRESS);

    long double expectedBits = 0.0L;
    int64_t startNs = 100'000;
    const auto recordQualifying = [&](Ptr<const WifiPsdu> psdu) {
        RecordPsdu(recorder, 7, startNs, psdu);
        expectedBits += static_cast<long double>(psdu->GetSize()) * 8.0L;
        startNs += 500'000;
    };

    recordQualifying(BuildPsdu(WIFI_MAC_DATA, 1000));
    recordQualifying(BuildPsdu(WIFI_MAC_DATA, 40));
    recordQualifying(BuildPsdu(WIFI_MAC_CTL_ACK, 0));
    recordQualifying(BuildPsdu(WIFI_MAC_CTL_BACKRESP, 24));
    recordQualifying(BuildPsdu(WIFI_MAC_CTL_BACKREQ, 16));
    recordQualifying(BuildPsdu(WIFI_MAC_CTL_RTS, 0));
    recordQualifying(BuildPsdu(WIFI_MAC_CTL_CTS, 0));

    const auto retry = BuildPsdu(WIFI_MAC_DATA, 600, ACCESS_POINT_ADDRESS, STATION_ADDRESS, true);
    recordQualifying(retry);
    recordQualifying(retry);

    const auto aggregate = BuildAggregatePsdu();
    RecordPsdu(recorder, 7, startNs, aggregate, BuildAggregateTxVector());
    expectedBits += static_cast<long double>(aggregate->GetSize()) * 8.0L;
    startNs += 500'000;

    const auto beforeExcluded = recorder.BuildOverallAccumulator(7);
    RecordPsdu(
        recorder,
        7,
        startNs,
        BuildPsdu(WIFI_MAC_MGT_BEACON, 80, Mac48Address::GetBroadcast(), ACCESS_POINT_ADDRESS));
    RecordPsdu(recorder, 7, startNs + 100'000, BuildPsdu(WIFI_MAC_MGT_ASSOCIATION_REQUEST, 80));
    RecordPsdu(
        recorder,
        7,
        startNs + 200'000,
        BuildPsdu(WIFI_MAC_MGT_PROBE_REQUEST, 80, Mac48Address::GetBroadcast(), STATION_ADDRESS));
    RecordPsdu(recorder,
               7,
               startNs + 300'000,
               BuildPsdu(WIFI_MAC_DATA, 500, Mac48Address::GetBroadcast(), STATION_ADDRESS));
    RecordPsdu(recorder, 7, startNs + 400'000, BuildPsdu(WIFI_MAC_MGT_ACTION, 80));
    RecordPsdu(recorder,
               7,
               startNs + 500'000,
               BuildPsdu(WIFI_MAC_DATA, 500, ACCESS_POINT_ADDRESS, UNRELATED_ADDRESS));
    RecordPsdu(recorder, 99, startNs + 600'000, BuildPsdu(WIFI_MAC_DATA, 500));

    const auto overall = recorder.BuildOverallAccumulator(7);
    NS_TEST_ASSERT_MSG_EQ_TOL(static_cast<double>(overall.psduBits),
                              static_cast<double>(expectedBits),
                              1e-12,
                              "Qualifying PSDU bytes or retransmission attempts were miscounted");
    NS_TEST_ASSERT_MSG_EQ(overall.ppduAirtimeNs,
                          beforeExcluded.ppduAirtimeNs,
                          "Excluded station frames contributed PPDU airtime");
    NS_TEST_ASSERT_MSG_EQ_TOL(static_cast<double>(overall.nominalRateBpsNs),
                              static_cast<double>(beforeExcluded.nominalRateBpsNs),
                              1e-12,
                              "Excluded station frames contributed nominal-rate airtime");

    bool duplicateRejected = false;
    try
    {
        recorder.RegisterStation(7, STATION_ADDRESS);
    }
    catch (const std::invalid_argument&)
    {
        duplicateRejected = true;
    }
    NS_TEST_ASSERT_MSG_EQ(duplicateRejected,
                          true,
                          "Duplicate station metric registration was accepted");
}

/** @ingroup tests Verify station control attribution follows the wire address layout. */
class StationPhyMetricControlAttributionTestCase : public TestCase
{
  public:
    StationPhyMetricControlAttributionTestCase();

  private:
    void DoRun() override;
};

StationPhyMetricControlAttributionTestCase::StationPhyMetricControlAttributionTestCase()
    : TestCase("attribute station control PPDUs by serialized transmitter address")
{
}

void
StationPhyMetricControlAttributionTestCase::DoRun()
{
    StationPhyMetricRecorder recorder(0, 20'000'000, 10'000'000);
    recorder.RegisterStation(7, STATION_ADDRESS);

    const auto ack = BuildPsdu(WIFI_MAC_CTL_ACK, 0, ACCESS_POINT_ADDRESS, UNRELATED_ADDRESS);
    const auto cts = BuildPsdu(WIFI_MAC_CTL_CTS, 0, ACCESS_POINT_ADDRESS, UNRELATED_ADDRESS);
    RecordPsdu(recorder, 7, 1'000'000, ack);
    RecordPsdu(recorder, 7, 2'000'000, cts);
    const auto oneAddressOverall = recorder.BuildOverallAccumulator(7);
    const long double expectedOneAddressBits =
        static_cast<long double>(ack->GetSize() + cts->GetSize()) * 8.0L;
    NS_TEST_ASSERT_MSG_EQ_TOL(
        static_cast<double>(oneAddressOverall.psduBits),
        static_cast<double>(expectedOneAddressBits),
        1e-12,
        "Recorder-owned station PHY binding did not admit one-address ACK/CTS frames");

    RecordPsdu(recorder,
               7,
               3'000'000,
               BuildPsdu(WIFI_MAC_CTL_RTS, 0, ACCESS_POINT_ADDRESS, UNRELATED_ADDRESS));
    RecordPsdu(recorder,
               7,
               4'000'000,
               BuildPsdu(WIFI_MAC_CTL_BACKREQ, 16, ACCESS_POINT_ADDRESS, UNRELATED_ADDRESS));
    RecordPsdu(recorder,
               7,
               5'000'000,
               BuildPsdu(WIFI_MAC_CTL_BACKRESP, 24, ACCESS_POINT_ADDRESS, UNRELATED_ADDRESS));

    const auto afterMismatchedTransmitters = recorder.BuildOverallAccumulator(7);
    NS_TEST_ASSERT_MSG_EQ(afterMismatchedTransmitters.ppduAirtimeNs,
                          oneAddressOverall.ppduAirtimeNs,
                          "Mismatched two-address control frame changed PPDU airtime");
    NS_TEST_ASSERT_MSG_EQ_TOL(static_cast<double>(afterMismatchedTransmitters.nominalRateBpsNs),
                              static_cast<double>(oneAddressOverall.nominalRateBpsNs),
                              1e-12,
                              "Mismatched two-address control frame changed nominal-rate airtime");
    NS_TEST_ASSERT_MSG_EQ_TOL(static_cast<double>(afterMismatchedTransmitters.psduBits),
                              static_cast<double>(oneAddressOverall.psduBits),
                              1e-12,
                              "Mismatched RTS/BAR/BlockAck frame changed PSDU bits");
}

/** @ingroup tests Verify partial A-MPDU attribution retains subframe overhead. */
class StationPhyMetricPartialAggregateTestCase : public TestCase
{
  public:
    StationPhyMetricPartialAggregateTestCase();

  private:
    void DoRun() override;
};

StationPhyMetricPartialAggregateTestCase::StationPhyMetricPartialAggregateTestCase()
    : TestCase("retain qualifying A-MPDU subframe delimiter and padding bytes")
{
}

void
StationPhyMetricPartialAggregateTestCase::DoRun()
{
    StationPhyMetricRecorder recorder(0, 20'000'000, 10'000'000);
    recorder.RegisterStation(7, STATION_ADDRESS);
    const auto aggregate = BuildPartiallyQualifyingAggregatePsdu();
    RecordPsdu(recorder, 7, 1'000'000, aggregate, BuildAggregateTxVector());

    const auto overall = recorder.BuildOverallAccumulator(7);
    NS_TEST_ASSERT_MSG_EQ(overall.psduBits,
                          2688.0L,
                          "Qualifying aggregate subframe lost delimiter or padding bits");
}

/** @ingroup tests Verify SU-only station metrics reject multi-user PPDU maps. */
class StationPhyMetricMultiUserRejectionTestCase : public TestCase
{
  public:
    StationPhyMetricMultiUserRejectionTestCase();

  private:
    void DoRun() override;
};

StationPhyMetricMultiUserRejectionTestCase::StationPhyMetricMultiUserRejectionTestCase()
    : TestCase("reject multi-user PPDU maps from SU-only station metrics")
{
}

void
StationPhyMetricMultiUserRejectionTestCase::DoRun()
{
    StationPhyMetricRecorder recorder(0, 20'000'000, 10'000'000);
    recorder.RegisterStation(7, STATION_ADDRESS);
    const auto txVector = BuildEqualRateTwoUserTxVector();
    const WifiConstPsduMap psduMap{
        {1, BuildPsdu(WIFI_MAC_DATA, 1400)},
        {2, BuildPsdu(WIFI_MAC_DATA, 1400)},
    };
    const int64_t durationNs =
        WifiPhy::CalculateTxDuration(psduMap, txVector, WIFI_PHY_BAND_5GHZ).GetNanoSeconds();
    const long double firstRate = txVector.GetMode(1).GetDataRate(txVector, 1);
    const long double secondRate = txVector.GetMode(2).GetDataRate(txVector, 2);
    NS_TEST_ASSERT_MSG_EQ(firstRate, secondRate, "Multi-user regression rates are not equal");

    StationPhyMetricAccumulator inconsistent;
    inconsistent.nominalRateBpsNs = firstRate * durationNs;
    inconsistent.psduBits =
        static_cast<long double>(psduMap.at(1)->GetSize() + psduMap.at(2)->GetSize()) * 8.0L;
    inconsistent.ppduAirtimeNs = durationNs;
    bool derivationRejected = false;
    try
    {
        static_cast<void>(DeriveStationPhyMetrics(inconsistent, 10'000'000));
    }
    catch (const std::invalid_argument&)
    {
        derivationRejected = true;
    }
    NS_TEST_ASSERT_MSG_EQ(derivationRejected,
                          true,
                          "Concurrent user bits did not expose the old rate inconsistency");

    bool observationRejected = false;
    std::string diagnostic;
    try
    {
        recorder.RecordPpduAttempt(7, 1'000'000, WIFI_PHY_BAND_5GHZ, psduMap, txVector);
    }
    catch (const std::invalid_argument& error)
    {
        observationRejected = true;
        diagnostic = error.what();
    }
    NS_TEST_ASSERT_MSG_EQ(observationRejected,
                          true,
                          "SU-only station metrics accepted a multi-user PPDU map");
    NS_TEST_ASSERT_MSG_EQ(diagnostic,
                          "station PHY metrics do not support multiple non-null PSDUs in SU mode",
                          "Multi-user rejection diagnostic is not precise");

    const auto overall = recorder.BuildOverallAccumulator(7);
    NS_TEST_ASSERT_MSG_EQ(overall.ppduAirtimeNs, 0, "Rejected multi-user PPDU changed airtime");
    NS_TEST_ASSERT_MSG_EQ(overall.psduBits, 0.0L, "Rejected multi-user PPDU changed PSDU bits");
}

/** @ingroup tests Verify nanosecond PPDU and contention boundary splitting. */
class StationPhyMetricWindowSplitTestCase : public TestCase
{
  public:
    StationPhyMetricWindowSplitTestCase();

  private:
    void DoRun() override;
};

StationPhyMetricWindowSplitTestCase::StationPhyMetricWindowSplitTestCase()
    : TestCase("split station PPDU and contention raw values across 10 ms windows")
{
}

void
StationPhyMetricWindowSplitTestCase::DoRun()
{
    constexpr int64_t windowNs = 10'000'000;
    StationPhyMetricRecorder recorder(0, 2 * windowNs, windowNs);
    recorder.RegisterStation(7, STATION_ADDRESS);
    const auto psdu = BuildPsdu(WIFI_MAC_DATA, 1400);
    const auto txVector = BuildLegacyTxVector(OfdmPhy::GetOfdmRate24Mbps());
    const WifiConstPsduMap psduMap{{SU_STA_ID, psdu}};
    const int64_t durationNs =
        WifiPhy::CalculateTxDuration(psduMap, txVector, WIFI_PHY_BAND_5GHZ).GetNanoSeconds();
    const int64_t firstOverlapNs = durationNs / 3;
    const int64_t secondOverlapNs = durationNs - firstOverlapNs;
    const int64_t startNs = windowNs - firstOverlapNs;

    recorder.RecordPpduAttempt(7, startNs, WIFI_PHY_BAND_5GHZ, psduMap, txVector);
    recorder.IngestContentionIntervals(7, {{windowNs - 100'000, windowNs + 200'000}});

    const auto& windows = recorder.GetWindowAccumulators(7);
    NS_TEST_ASSERT_MSG_EQ(windows.size(), 2, "Wrong station window count");
    NS_TEST_ASSERT_MSG_EQ(windows[0].ppduAirtimeNs,
                          firstOverlapNs,
                          "Wrong first-window PPDU overlap");
    NS_TEST_ASSERT_MSG_EQ(windows[1].ppduAirtimeNs,
                          secondOverlapNs,
                          "Wrong second-window PPDU overlap");
    NS_TEST_ASSERT_MSG_EQ(windows[0].contentionNs,
                          100'000,
                          "Wrong first-window contention overlap");
    NS_TEST_ASSERT_MSG_EQ(windows[1].contentionNs,
                          200'000,
                          "Wrong second-window contention overlap");

    const long double completeBits = static_cast<long double>(psdu->GetSize()) * 8.0L;
    const long double nominalRateBps = txVector.GetMode(SU_STA_ID).GetDataRate(txVector, SU_STA_ID);
    NS_TEST_ASSERT_MSG_EQ_TOL(static_cast<double>(windows[0].psduBits),
                              static_cast<double>(completeBits * firstOverlapNs / durationNs),
                              1e-12,
                              "First-window PSDU bits were not allocated by nanosecond overlap");
    NS_TEST_ASSERT_MSG_EQ_TOL(static_cast<double>(windows[1].psduBits),
                              static_cast<double>(completeBits * secondOverlapNs / durationNs),
                              1e-12,
                              "Second-window PSDU bits were not allocated by nanosecond overlap");
    NS_TEST_ASSERT_MSG_EQ_TOL(static_cast<double>(windows[0].nominalRateBpsNs),
                              static_cast<double>(nominalRateBps * firstOverlapNs),
                              1e-3,
                              "First-window nominal-rate product was not split by overlap");
    NS_TEST_ASSERT_MSG_EQ_TOL(static_cast<double>(windows[1].nominalRateBpsNs),
                              static_cast<double>(nominalRateBps * secondOverlapNs),
                              1e-3,
                              "Second-window nominal-rate product was not split by overlap");

    StationPhyMetricAccumulator merged;
    merged.Merge(windows[0]);
    merged.Merge(windows[1]);
    const auto overall = recorder.BuildOverallAccumulator(7);
    NS_TEST_ASSERT_MSG_EQ(overall.ppduAirtimeNs,
                          durationNs,
                          "Window PPDU airtime did not merge to complete duration");
    NS_TEST_ASSERT_MSG_EQ(overall.contentionNs,
                          300'000,
                          "Window contention did not merge to the interval union");
    NS_TEST_ASSERT_MSG_EQ_TOL(static_cast<double>(overall.nominalRateBpsNs),
                              static_cast<double>(merged.nominalRateBpsNs),
                              1e-3,
                              "Overall nominal-rate product was not rebuilt from windows");
    NS_TEST_ASSERT_MSG_EQ_TOL(static_cast<double>(overall.psduBits),
                              static_cast<double>(merged.psduBits),
                              1e-12,
                              "Overall PSDU bits were not rebuilt from windows");
    NS_TEST_ASSERT_MSG_EQ_TOL(static_cast<double>(overall.psduBits),
                              static_cast<double>(completeBits),
                              1e-12,
                              "Proportional window PSDU bits did not merge to the PPDU total");
}

} // namespace

std::vector<TestCase*>
CreateSaturatedTcpStaPhyMetricTestCases()
{
    return {
        new StationPhyMetricFormulaTestCase(),
        new StationPhyMetricRangeTestCase(),
        new StationPhyMetricClassificationTestCase(),
        new StationPhyMetricControlAttributionTestCase(),
        new StationPhyMetricPartialAggregateTestCase(),
        new StationPhyMetricMultiUserRejectionTestCase(),
        new StationPhyMetricWindowSplitTestCase(),
    };
}
