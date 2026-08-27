#include "../../examples/saturated-tcp/data-tx-metrics-internal.h"

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

#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace ns3;

namespace
{

const Mac48Address STATION_ADDRESS("00:00:00:00:00:02");
const Mac48Address ACCESS_POINT_ADDRESS("00:00:00:00:00:01");

WifiTxVector
BuildHeTxVector(WifiMode mode = HePhy::GetHeMcs9(),
                uint8_t nss = 1,
                MHz_u channelWidth = MHz_u{80},
                Time guardInterval = NanoSeconds(3200),
                bool aggregation = false)
{
    return WifiTxVector(mode,
                        WIFI_MIN_TX_PWR_LEVEL,
                        WIFI_PREAMBLE_HE_SU,
                        guardInterval,
                        nss,
                        nss,
                        0,
                        channelWidth,
                        aggregation);
}

WifiTxVector
BuildLegacyTxVector()
{
    return WifiTxVector(OfdmPhy::GetOfdmRate54Mbps(),
                        WIFI_MIN_TX_PWR_LEVEL,
                        WIFI_PREAMBLE_LONG,
                        NanoSeconds(800),
                        1,
                        1,
                        0,
                        MHz_u{20},
                        false);
}

WifiTxVector
BuildOneUserHeMuTxVector()
{
    WifiTxVector txVector(HePhy::GetHeMcs9(),
                          WIFI_MIN_TX_PWR_LEVEL,
                          WIFI_PREAMBLE_HE_MU,
                          NanoSeconds(3200),
                          1,
                          1,
                          0,
                          MHz_u{80},
                          false);
    txVector.SetHeMuUserInfo(1, {HeRu::RuSpec{RuType::RU_106_TONE, 1, true}, 9, 1});
    txVector.SetSigBMode(VhtPhy::GetVhtMcs0());
    return txVector;
}

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

std::optional<DataTxProfileContribution>
Extract(Ptr<const WifiPsdu> psdu, const WifiTxVector& txVector = BuildHeTxVector())
{
    return ExtractDataTxProfileContribution(STATION_ADDRESS,
                                            WIFI_PHY_BAND_5GHZ,
                                            {{SU_STA_ID, psdu}},
                                            txVector);
}

void
Record(StationDataTxMetricRecorder& recorder,
       int64_t startNs,
       Ptr<const WifiPsdu> psdu,
       const WifiTxVector& txVector = BuildHeTxVector())
{
    recorder.RecordPpduAttempt(7, startNs, WIFI_PHY_BAND_5GHZ, {{SU_STA_ID, psdu}}, txVector);
}

template <typename Function>
bool
ThrowsInvalidArgument(Function&& function)
{
    try
    {
        function();
    }
    catch (const std::invalid_argument&)
    {
        return true;
    }
    return false;
}

template <typename Function>
std::string
GetInvalidArgumentMessage(Function&& function)
{
    try
    {
        function();
    }
    catch (const std::invalid_argument& error)
    {
        return error.what();
    }
    return {};
}

/** @ingroup tests Catch payload-size filters and incomplete PSDU byte accounting. */
class DataTxMetricDataFrameTestCase : public TestCase
{
  public:
    DataTxMetricDataFrameTestCase()
        : TestCase("count full HE data and TCP-ACK-sized PSDUs; catches payload-size filtering")
    {
    }

  private:
    void DoRun() override
    {
        const auto data = Extract(BuildPsdu(WIFI_MAC_DATA, 1000));
        NS_TEST_ASSERT_MSG_EQ(data.has_value(), true, "Unicast station data was excluded");
        NS_TEST_ASSERT_MSG_EQ(+data->key.nss, 1, "Single-stream data used the wrong profile");
        NS_TEST_ASSERT_MSG_EQ(+data->key.mcs, 9, "HE MCS 9 data used the wrong profile");
        NS_TEST_ASSERT_MSG_EQ(data->transmittedPsduBytes,
                              1028.0L,
                              "Data count omitted MAC header or FCS bytes");
        NS_TEST_ASSERT_MSG_EQ(data->nominalRateBps,
                              408'333'334.0L,
                              "HE 80 MHz GI 3200 NSS1 MCS9 rate drifted");
        NS_TEST_ASSERT_MSG_EQ(data->ppduAirtimeNs, 76'000, "Complete HE data PPDU airtime drifted");

        const auto tcpAckLike = Extract(BuildPsdu(WIFI_MAC_DATA, 40));
        NS_TEST_ASSERT_MSG_EQ(tcpAckLike.has_value(),
                              true,
                              "TCP-ACK-sized Wi-Fi data was payload-filtered");
        NS_TEST_ASSERT_MSG_EQ(tcpAckLike->transmittedPsduBytes,
                              68.0L,
                              "TCP-ACK-like PSDU omitted MAC bytes");
        NS_TEST_ASSERT_MSG_EQ(tcpAckLike->ppduAirtimeNs,
                              60'000,
                              "Complete TCP-ACK-like PPDU airtime drifted");
    }
};

/** @ingroup tests Catch removal of data type and station address filters. */
class DataTxMetricExclusionTestCase : public TestCase
{
  public:
    DataTxMetricExclusionTestCase()
        : TestCase(
              "exclude control, management, group, and AP data; catches address filter removal")
    {
    }

  private:
    void DoRun() override
    {
        const std::vector<Ptr<const WifiPsdu>> excluded{
            BuildPsdu(WIFI_MAC_CTL_ACK, 0),
            BuildPsdu(WIFI_MAC_CTL_BACKRESP, 24),
            BuildPsdu(WIFI_MAC_CTL_RTS, 0),
            BuildPsdu(WIFI_MAC_CTL_CTS, 0),
            BuildPsdu(WIFI_MAC_MGT_ASSOCIATION_REQUEST, 80),
            BuildPsdu(WIFI_MAC_DATA, 500, Mac48Address::GetBroadcast()),
            BuildPsdu(WIFI_MAC_DATA, 500, ACCESS_POINT_ADDRESS, ACCESS_POINT_ADDRESS),
        };
        for (const auto& psdu : excluded)
        {
            NS_TEST_ASSERT_MSG_EQ(Extract(psdu).has_value(),
                                  false,
                                  "Non-station unicast data entered a TX profile");
        }
    }
};

/** @ingroup tests Catch A-MPDU overhead loss, deduplication, and profile-key collapse. */
class DataTxMetricAggregateAndProfileTestCase : public TestCase
{
  public:
    DataTxMetricAggregateAndProfileTestCase()
        : TestCase("retain A-MPDU bytes and separate NSS-MCS; catches profile-key collapse")
    {
    }

  private:
    void DoRun() override
    {
        StationDataTxMetricRecorder recorder(0, 20'000'000, 10'000'000);
        recorder.RegisterStation(7, STATION_ADDRESS);
        const auto retry =
            BuildPsdu(WIFI_MAC_DATA, 1000, ACCESS_POINT_ADDRESS, STATION_ADDRESS, true);
        Record(recorder, 1'000'000, retry);
        Record(recorder, 2'000'000, retry);

        const auto aggregate = BuildAggregatePsdu();
        NS_TEST_ASSERT_MSG_EQ(aggregate->GetSize(),
                              870,
                              "A-MPDU fixture no longer has hand-derived subframe bytes");
        const auto nss2Mcs11 =
            BuildHeTxVector(HePhy::GetHeMcs11(), 2, MHz_u{80}, NanoSeconds(3200), true);
        Record(recorder, 3'000'000, aggregate, nss2Mcs11);

        const auto& profiles = recorder.GetOverallProfiles(7);
        NS_TEST_ASSERT_MSG_EQ(profiles.size(), 2, "NSS/MCS attempts collapsed into one profile");
        const auto& first = profiles.at({1, 9});
        NS_TEST_ASSERT_MSG_EQ(first.transmittedPsduBytes,
                              2056.0L,
                              "Repeated data attempt was deduplicated");
        NS_TEST_ASSERT_MSG_EQ(first.ppduAttemptCount,
                              2,
                              "Repeated data attempt was not counted twice");
        NS_TEST_ASSERT_MSG_EQ(first.nominalRateBps,
                              408'333'334.0L,
                              "NSS1/MCS9 nominal rate drifted");
        const auto& second = profiles.at({2, 11});
        NS_TEST_ASSERT_MSG_EQ(second.transmittedPsduBytes,
                              870.0L,
                              "A-MPDU delimiter or padding bytes were lost");
        NS_TEST_ASSERT_MSG_EQ(second.ppduAttemptCount, 1, "A-MPDU was counted more than once");
        NS_TEST_ASSERT_MSG_EQ(second.nominalRateBps,
                              1'020'833'334.0L,
                              "NSS2/MCS11 nominal rate drifted");
    }
};

/** @ingroup tests Catch bypasses of fixed PHY, SU-shape, and duration validation. */
class DataTxMetricInvariantTestCase : public TestCase
{
  public:
    DataTxMetricInvariantTestCase()
        : TestCase("reject PHY invariant and duration violations; catches validation bypass")
    {
    }

  private:
    void DoRun() override
    {
        const auto data = BuildPsdu(WIFI_MAC_DATA, 1000);
        NS_TEST_ASSERT_MSG_EQ(ThrowsInvalidArgument([&] { Extract(data, BuildLegacyTxVector()); }),
                              true,
                              "Qualifying non-HE data was accepted");
        NS_TEST_ASSERT_MSG_EQ(ThrowsInvalidArgument([&] {
                                  Extract(data, BuildHeTxVector(HePhy::GetHeMcs9(), 1, MHz_u{40}));
                              }),
                              true,
                              "Qualifying HE data with the wrong width was accepted");
        NS_TEST_ASSERT_MSG_EQ(
            ThrowsInvalidArgument([&] {
                Extract(data, BuildHeTxVector(HePhy::GetHeMcs9(), 1, MHz_u{80}, NanoSeconds(1600)));
            }),
            true,
            "Qualifying HE data with the wrong guard interval was accepted");

        const WifiConstPsduMap twoPsdus{{1, data}, {2, data}};
        NS_TEST_ASSERT_MSG_EQ(ThrowsInvalidArgument([&] {
                                  ExtractDataTxProfileContribution(STATION_ADDRESS,
                                                                   WIFI_PHY_BAND_5GHZ,
                                                                   twoPsdus,
                                                                   BuildHeTxVector());
                              }),
                              true,
                              "Multiple non-null SU PSDUs were accepted");
        NS_TEST_ASSERT_MSG_EQ(
            ThrowsInvalidArgument([] { StationDataTxMetricRecorder recorder(0, 0, 10'000'000); }),
            true,
            "Empty measurement duration was accepted");
        NS_TEST_ASSERT_MSG_EQ(ThrowsInvalidArgument([] {
                                  StationDataTxMetricRecorder recorder(0, 1'000'000'000, 0);
                              }),
                              true,
                              "Zero window duration was accepted");
        NS_TEST_ASSERT_MSG_EQ(
            ThrowsInvalidArgument(
                [] { StationDataTxMetricRecorder recorder(0, 1'000'000'001, 10'000'000); }),
            true,
            "Partial final window was accepted");

        StationDataTxMetricRecorder recorder(0, 1'000'000'000, 10'000'000);
        recorder.RegisterStation(7, STATION_ADDRESS);
        NS_TEST_ASSERT_MSG_EQ(ThrowsInvalidArgument([&] {
                                  Record(recorder, std::numeric_limits<int64_t>::max() - 1, data);
                              }),
                              true,
                              "Overflowing PPDU duration was accepted");
    }
};

/** @ingroup tests Catch admission of MU vectors or AID-keyed SU PSDUs. */
class DataTxMetricSuScopeTestCase : public TestCase
{
  public:
    DataTxMetricSuScopeTestCase()
        : TestCase("reject HE-MU and non-SU-key data; catches SU-shape validation removal")
    {
    }

  private:
    void DoRun() override
    {
        const auto data = BuildPsdu(WIFI_MAC_DATA, 1000);
        const WifiConstPsduMap aidKeyedPsdu{{1, data}};
        NS_TEST_ASSERT_MSG_EQ(GetInvalidArgumentMessage([&] {
                                  ExtractDataTxProfileContribution(STATION_ADDRESS,
                                                                   WIFI_PHY_BAND_5GHZ,
                                                                   aidKeyedPsdu,
                                                                   BuildOneUserHeMuTxVector());
                              }),
                              "data TX profiles require an HE SU transmission vector",
                              "One-user HE-MU data bypassed the transmission-vector shape guard");
        NS_TEST_ASSERT_MSG_EQ(GetInvalidArgumentMessage([&] {
                                  ExtractDataTxProfileContribution(STATION_ADDRESS,
                                                                   WIFI_PHY_BAND_5GHZ,
                                                                   aidKeyedPsdu,
                                                                   BuildHeTxVector());
                              }),
                              "data TX profiles require the SU_STA_ID PSDU key",
                              "A non-SU PSDU key bypassed the SU-key shape guard");
    }
};

/** @ingroup tests Catch whole-PPDU allocation and duplicate attempt attribution. */
class DataTxMetricWindowOverallTestCase : public TestCase
{
  public:
    DataTxMetricWindowOverallTestCase()
        : TestCase("split bytes at boundaries and count starts once; catches whole-PPDU allocation")
    {
    }

  private:
    void DoRun() override
    {
        constexpr int64_t WINDOW_NS = 10'000'000;
        constexpr int64_t EPOCH_NS = 1'000'000'000;
        constexpr int64_t PPDU_NS = 76'000;
        StationDataTxMetricRecorder recorder(0, EPOCH_NS, WINDOW_NS);
        recorder.RegisterStation(7, STATION_ADDRESS);
        const auto data = BuildPsdu(WIFI_MAC_DATA, 1000);
        Record(recorder, WINDOW_NS - PPDU_NS / 2, data);

        const auto& windows = recorder.GetWindowProfiles(7);
        const auto& first = windows.at(0).at({1, 9});
        const auto& second = windows.at(1).at({1, 9});
        NS_TEST_ASSERT_MSG_EQ(first.transmittedPsduBytes,
                              514.0L,
                              "First window did not receive proportional bytes");
        NS_TEST_ASSERT_MSG_EQ(second.transmittedPsduBytes,
                              514.0L,
                              "Second window did not receive proportional bytes");
        NS_TEST_ASSERT_MSG_EQ(first.ppduAirtimeNs,
                              38'000,
                              "First window did not receive exact overlap airtime");
        NS_TEST_ASSERT_MSG_EQ(second.ppduAirtimeNs,
                              38'000,
                              "Second window did not receive exact overlap airtime");
        NS_TEST_ASSERT_MSG_EQ(first.ppduAttemptCount,
                              1,
                              "Attempt was not assigned to its PPDU start window");
        NS_TEST_ASSERT_MSG_EQ(second.ppduAttemptCount,
                              0,
                              "Attempt was duplicated into the PPDU end window");

        DataTxProfileAccumulator merged;
        merged.Merge(first);
        merged.Merge(second);
        const auto& overallBeforeClip = recorder.GetOverallProfiles(7).at({1, 9});
        NS_TEST_ASSERT_MSG_EQ(overallBeforeClip.transmittedPsduBytes,
                              merged.transmittedPsduBytes,
                              "Independent overall bytes differ from merged windows");
        NS_TEST_ASSERT_MSG_EQ(overallBeforeClip.ppduAttemptCount,
                              1,
                              "Independent overall duplicated a boundary-crossing attempt");
        NS_TEST_ASSERT_MSG_EQ(overallBeforeClip.ppduAirtimeNs,
                              merged.ppduAirtimeNs,
                              "Independent overall airtime differs from merged windows");
        NS_TEST_ASSERT_MSG_EQ(overallBeforeClip.nominalRateBps,
                              merged.nominalRateBps,
                              "Profile merge changed the nominal rate");

        Record(recorder, EPOCH_NS - PPDU_NS / 2, data);
        const auto& final = windows.at(99).at({1, 9});
        NS_TEST_ASSERT_MSG_EQ(final.transmittedPsduBytes,
                              514.0L,
                              "One-second endpoint did not clip proportional bytes");
        NS_TEST_ASSERT_MSG_EQ(final.ppduAirtimeNs,
                              38'000,
                              "One-second endpoint did not clip PPDU airtime");
        NS_TEST_ASSERT_MSG_EQ(final.ppduAttemptCount,
                              1,
                              "Clipped PPDU start was not counted in the final window");
        const auto& overall = recorder.GetOverallProfiles(7).at({1, 9});
        NS_TEST_ASSERT_MSG_EQ(overall.transmittedPsduBytes,
                              1542.0L,
                              "Independent overall did not apply endpoint clipping");
        NS_TEST_ASSERT_MSG_EQ(overall.ppduAirtimeNs,
                              114'000,
                              "Independent overall airtime did not apply endpoint clipping");
        NS_TEST_ASSERT_MSG_EQ(overall.ppduAttemptCount,
                              2,
                              "Independent overall did not count each PPDU start exactly once");
    }
};

} // namespace

std::vector<TestCase*>
CreateSaturatedTcpDataTxMetricTestCases()
{
    return {
        new DataTxMetricDataFrameTestCase(),
        new DataTxMetricExclusionTestCase(),
        new DataTxMetricAggregateAndProfileTestCase(),
        new DataTxMetricInvariantTestCase(),
        new DataTxMetricSuScopeTestCase(),
        new DataTxMetricWindowOverallTestCase(),
    };
}
