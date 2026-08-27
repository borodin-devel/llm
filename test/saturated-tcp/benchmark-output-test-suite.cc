#include "../../examples/saturated-tcp/benchmark-statistics.h"
#include "../../examples/saturated-tcp/config.h"
#include "../llm-test-suite.h"

#include "ns3/boolean.h"
#include "ns3/enum.h"
#include "ns3/json.hpp"
#include "ns3/node.h"
#include "ns3/pointer.h"
#include "ns3/qos-txop.h"
#include "ns3/simulator.h"
#include "ns3/sta-wifi-mac.h"
#include "ns3/wifi-net-device.h"
#include "ns3/yans-wifi-phy.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <string_view>
#include <vector>

using namespace ns3;

namespace
{

using RawStationWindows = std::map<uint32_t, std::vector<DataTxProfileMap>>;
using RawStationOverall = std::map<uint32_t, DataTxProfileMap>;

class RejectingStreamBuffer : public std::streambuf
{
  private:
    std::streamsize xsputn(const char*, std::streamsize) override
    {
        return 0;
    }

    int_type overflow(int_type) override
    {
        return traits_type::eof();
    }
};

void
SetRawProfile(RawStationWindows& windows,
              uint32_t nodeId,
              std::size_t windowIndex,
              DataTxProfileKey key,
              long double bytes,
              uint64_t attempts,
              int64_t airtimeNs,
              long double rateBps)
{
    windows.at(nodeId).at(windowIndex)[key] = {bytes, attempts, airtimeNs, rateBps};
}

RawStationOverall
MergeRawWindows(const RawStationWindows& windows)
{
    RawStationOverall overall;
    for (const auto& [nodeId, stationWindows] : windows)
    {
        auto& stationOverall = overall[nodeId];
        for (const auto& window : stationWindows)
        {
            for (const auto& [key, value] : window)
            {
                stationOverall[key].Merge(value);
            }
        }
    }
    return overall;
}

const StationStatisticsOutput&
FindStation(const std::vector<StationStatisticsOutput>& stations,
            uint32_t accessPointId,
            uint32_t stationIndex)
{
    for (const auto& station : stations)
    {
        if (station.accessPointId == accessPointId && station.stationIndex == stationIndex)
        {
            return station;
        }
    }
    throw std::runtime_error("station fixture is missing");
}

const AccessPointStatisticsOutput&
FindAccessPoint(const std::vector<AccessPointStatisticsOutput>& accessPoints,
                uint32_t accessPointId)
{
    for (const auto& accessPoint : accessPoints)
    {
        if (accessPoint.accessPointId == accessPointId)
        {
            return accessPoint;
        }
    }
    throw std::runtime_error("access point fixture is missing");
}

PhyCategoryOutput
MakeStationPhy(double bytes, uint64_t attempts, double airtimeUs, int64_t intervalNs)
{
    DataTxProfileMap profiles{
        {{80, 2, 11},
         {bytes, attempts, static_cast<int64_t>(airtimeUs * 1000.0), 1'000'000'000.0L}}};
    return DeriveStationDataTxMetrics(profiles, intervalNs);
}

UnifiedExperimentSummary
MakeLiteralSummary()
{
    UnifiedExperimentSummary summary;
    summary.statisticsWindowMs = 10;
    ExperimentWindowOutput window{0, 0.0, 10.0, {}, {}};
    for (uint32_t accessPointId = 0; accessPointId < 3; ++accessPointId)
    {
        const uint32_t apNodeId = 100 + accessPointId;
        const uint32_t stationNodeId = 200 + accessPointId;
        ExperimentEntityIdentity apIdentity{ExperimentEntityKind::ACCESS_POINT,
                                            accessPointId,
                                            std::nullopt,
                                            apNodeId,
                                            "AP" + std::to_string(accessPointId),
                                            "10.1." + std::to_string(accessPointId) + ".1"};
        ExperimentEntityIdentity stationIdentity{ExperimentEntityKind::STATION,
                                                 accessPointId,
                                                 0,
                                                 stationNodeId,
                                                 "AP" + std::to_string(accessPointId) + "/STA0",
                                                 "10.1." + std::to_string(accessPointId) + ".2"};
        summary.accessPointInventory.push_back(apIdentity);
        summary.stationInventory.push_back(stationIdentity);

        const auto windowPhy = MakeStationPhy(1000.0 * (accessPointId + 1), 2, 100.0, 10'000'000);
        const auto overallPhy =
            MakeStationPhy(1000.0 * (accessPointId + 1), 2, 100.0, 1'000'000'000);
        StationStatisticsOutput windowStation{accessPointId,
                                              0,
                                              stationNodeId,
                                              stationIdentity.nodeLabel,
                                              stationIdentity.ipv4,
                                              {}};
        windowStation.statistics.phyStats = windowPhy;
        StationStatisticsOutput overallStation = windowStation;
        overallStation.statistics.phyStats = overallPhy;
        AccessPointStatisticsOutput windowAp{accessPointId,
                                             apNodeId,
                                             apIdentity.nodeLabel,
                                             apIdentity.ipv4,
                                             {}};
        windowAp.statistics.phyStats = DeriveBssDataTxMetrics({windowPhy});
        AccessPointStatisticsOutput overallAp = windowAp;
        overallAp.statistics.phyStats = DeriveBssDataTxMetrics({overallPhy});
        window.accessPoints.push_back(windowAp);
        window.stations.push_back(windowStation);
        summary.overall.accessPoints.push_back(overallAp);
        summary.overall.stations.push_back(overallStation);
    }
    summary.windows.push_back(window);
    return summary;
}

Ptr<WifiNetDevice>
MakeStationDevice(Ptr<Node> node, bool omitPhy = false)
{
    const auto makeQosTxop = [](AcIndex ac) {
        return CreateObjectWithAttributes<QosTxop>("AcIndex", EnumValue(ac));
    };
    auto mac = CreateObjectWithAttributes<StaWifiMac>("QosSupported",
                                                      BooleanValue(true),
                                                      "BE_Txop",
                                                      PointerValue(makeQosTxop(AC_BE)),
                                                      "BK_Txop",
                                                      PointerValue(makeQosTxop(AC_BK)),
                                                      "VI_Txop",
                                                      PointerValue(makeQosTxop(AC_VI)),
                                                      "VO_Txop",
                                                      PointerValue(makeQosTxop(AC_VO)));
    mac->SetAddress(Mac48Address::Allocate());
    auto device = CreateObject<WifiNetDevice>();
    device->SetMac(mac);
    if (!omitPhy)
    {
        device->SetPhy(CreateObject<YansWifiPhy>());
    }
    node->AddDevice(device);
    return device;
}

std::vector<std::string>
GetKeys(const nlohmann::ordered_json& object)
{
    std::vector<std::string> keys;
    for (const auto& [key, value] : object.items())
    {
        static_cast<void>(value);
        keys.push_back(key);
    }
    return keys;
}

} // namespace

/** @ingroup tests Verify profile formulas, tie-breaks, raw validation, and BSS aggregation. */
class SaturatedTcpDataTxDerivationTestCase : public TestCase
{
  public:
    SaturatedTcpDataTxDerivationTestCase()
        : TestCase("derive saturated station and BSS data TX metrics")
    {
    }

  private:
    void DoRun() override
    {
        DataTxProfileMap profiles{
            {{20, 1, 9}, {100.0L, 2, 40'000, 400'000'000.0L}},
            {{80, 2, 11}, {300.0L, 4, 120'000, 1'000'000'000.0L}},
        };
        const auto output = DeriveStationDataTxMetrics(profiles, 10'000'000);
        NS_TEST_ASSERT_MSG_EQ_TOL(output.dominantDataPhyRateMbps.value(),
                                  1000.0,
                                  1e-12,
                                  "Greatest-byte profile was not dominant");
        NS_TEST_ASSERT_MSG_EQ_TOL(output.dominantDataProfileShare.value(),
                                  0.75,
                                  1e-12,
                                  "Dominant byte share is wrong");
        NS_TEST_ASSERT_MSG_EQ_TOL(output.effectivePhyRateMbps.value(),
                                  20.0,
                                  1e-12,
                                  "Effective rate is not 8B/Tdata");
        NS_TEST_ASSERT_MSG_EQ_TOL(output.dataTxRateOverIntervalMbps.value(),
                                  0.32,
                                  1e-12,
                                  "Interval rate is not 8B/Tinterval");
        NS_TEST_ASSERT_MSG_EQ_TOL(output.dataTxOpportunityGapFraction.value(),
                                  0.984,
                                  1e-12,
                                  "Opportunity gap is wrong");
        NS_TEST_ASSERT_MSG_EQ(+output.dataTxProfile.front().channelWidthMhz,
                              20,
                              "Profiles are not ascending by width");
        NS_TEST_ASSERT_MSG_EQ(+output.dataTxProfile.front().nss,
                              1,
                              "Profiles are not ascending by NSS");

        profiles.at({20, 1, 9}).transmittedPsduBytes = 300.0L;
        profiles.at({20, 1, 9}).nominalRateBps = 900'000'000.0L;
        NS_TEST_ASSERT_MSG_EQ_TOL(
            DeriveStationDataTxMetrics(profiles, 10'000'000).dominantDataPhyRateMbps.value(),
            1000.0,
            1e-12,
            "Byte tie did not prefer greatest nominal rate");
        profiles.at({20, 1, 9}).nominalRateBps = 1'000'000'000.0L;
        NS_TEST_ASSERT_MSG_EQ_TOL(
            DeriveStationDataTxMetrics(profiles, 10'000'000).dominantDataPhyRateMbps.value(),
            1000.0,
            1e-12,
            "Exact tie changed the ascending-key dominant rate");

        const auto zero = DeriveStationDataTxMetrics({}, 10'000'000);
        NS_TEST_ASSERT_MSG_EQ(zero.dominantDataPhyRateMbps.has_value(), false, "Zero has dominant");
        NS_TEST_ASSERT_MSG_EQ(zero.effectivePhyRateMbps.has_value(), false, "Zero has effective");
        NS_TEST_ASSERT_MSG_EQ(zero.dataTxRateOverIntervalMbps.value(), 0.0, "Zero lacks interval");
        NS_TEST_ASSERT_MSG_EQ(zero.dataTxProfile.empty(), true, "Zero has profile entries");

        auto second = output;
        second.dominantDataPhyRateMbps = 500.0;
        second.effectivePhyRateMbps = 10.0;
        second.dataTxRateOverIntervalMbps = 0.08;
        const auto bss = DeriveBssDataTxMetrics({output, second, zero});
        NS_TEST_ASSERT_MSG_EQ_TOL(bss.meanDominantDataPhyRateMbps.value(),
                                  750.0,
                                  1e-12,
                                  "BSS dominant mean included undefined station");
        NS_TEST_ASSERT_MSG_EQ_TOL(bss.meanEffectivePhyRateMbps.value(),
                                  15.0,
                                  1e-12,
                                  "BSS effective mean included undefined station");
        NS_TEST_ASSERT_MSG_EQ_TOL(bss.aggregateDataTxRateOverIntervalMbps.value(),
                                  0.4,
                                  1e-12,
                                  "BSS interval sum omitted a station");
        const auto idleBss = DeriveBssDataTxMetrics({zero, zero});
        NS_TEST_ASSERT_MSG_EQ(idleBss.meanDominantDataPhyRateMbps.has_value(),
                              false,
                              "Idle BSS has dominant mean");
        NS_TEST_ASSERT_MSG_EQ(idleBss.aggregateDataTxRateOverIntervalMbps.value(),
                              0.0,
                              "Idle BSS lacks numeric zero aggregate");

        for (const auto invalid : {
                 DataTxProfileAccumulator{-1.0L, 1, 1, 1.0L},
                 DataTxProfileAccumulator{1.0L, 1, -1, 1.0L},
                 DataTxProfileAccumulator{1.0L, 1, 1, std::numeric_limits<long double>::infinity()},
             })
        {
            bool rejected = false;
            try
            {
                static_cast<void>(DeriveStationDataTxMetrics({{{80, 1, 0}, invalid}}, 10'000'000));
            }
            catch (const std::invalid_argument&)
            {
                rejected = true;
            }
            NS_TEST_ASSERT_MSG_EQ(rejected, true, "Invalid raw profile was accepted");
        }
    }
};

/** @ingroup tests Verify sparse windows, dense overall, and BSS derivation. */
class SaturatedTcpBenchmarkSummaryTestCase : public TestCase
{
  public:
    SaturatedTcpBenchmarkSummaryTestCase();

  private:
    void DoRun() override;
};

/** @ingroup tests Verify ordinary station trace ownership and idle lifecycle. */
class SaturatedTcpBenchmarkLifecycleTestCase : public TestCase
{
  public:
    SaturatedTcpBenchmarkLifecycleTestCase();

  private:
    void DoRun() override;
};

SaturatedTcpBenchmarkSummaryTestCase::SaturatedTcpBenchmarkSummaryTestCase()
    : TestCase("build sparse data-only windows and dense station-derived BSS summaries")
{
}

void
SaturatedTcpBenchmarkSummaryTestCase::DoRun()
{
    SaturatedTcpStatistics statistics(10);
    for (uint32_t accessPointId = 0; accessPointId < 3; ++accessPointId)
    {
        statistics.RegisterAccessPoint(accessPointId,
                                       100 + accessPointId,
                                       "AP" + std::to_string(accessPointId),
                                       "10.1." + std::to_string(accessPointId) + ".1");
    }
    statistics.RegisterStation(0, 0, 10, "AP0/STA0", "10.1.0.2");
    statistics.RegisterStation(0, 1, 11, "AP0/STA1", "10.1.0.3");
    statistics.RegisterStation(0, 2, 12, "AP0/STA2", "10.1.0.4");
    statistics.RegisterStation(1, 0, 20, "AP1/STA0", "10.1.1.2");
    statistics.RegisterStation(2, 0, 30, "AP2/STA0", "10.1.2.2");

    RawStationWindows rawWindows;
    for (const uint32_t nodeId : {10, 11, 12, 20, 30, 100})
    {
        rawWindows.emplace(nodeId, std::vector<DataTxProfileMap>(100));
    }
    SetRawProfile(rawWindows, 10, 0, {20, 1, 9}, 1000.0L, 2, 100'000, 400'000'000.0L);
    SetRawProfile(rawWindows, 11, 0, {80, 2, 11}, 2000.0L, 3, 200'000, 800'000'000.0L);
    SetRawProfile(rawWindows, 11, 1, {40, 1, 9}, 500.0L, 1, 50'000, 400'000'000.0L);
    SetRawProfile(rawWindows, 30, 0, {80, 2, 11}, 3000.0L, 4, 300'000, 1'000'000'000.0L);
    SetRawProfile(rawWindows, 100, 0, {80, 2, 11}, 9000.0L, 9, 900'000, 1'000'000'000.0L);

    const auto summary = statistics.BuildSummaryFromRaw(rawWindows);
    NS_TEST_ASSERT_MSG_EQ(summary.windows.size(), 2, "Inactive windows were not sparse");
    NS_TEST_ASSERT_MSG_EQ(summary.windows.at(0).stations.size(), 3, "Active stations were lost");
    NS_TEST_ASSERT_MSG_EQ(summary.windows.at(0).accessPoints.size(), 2, "Inactive BSS was emitted");
    NS_TEST_ASSERT_MSG_EQ(summary.windows.at(1).stations.size(), 1, "Inactive station was emitted");
    const auto& bss0 = FindAccessPoint(summary.windows.at(0).accessPoints, 0).statistics.phyStats;
    NS_TEST_ASSERT_MSG_EQ_TOL(bss0.meanDominantDataPhyRateMbps.value(),
                              600.0,
                              1e-12,
                              "BSS dominant mean is wrong");
    NS_TEST_ASSERT_MSG_EQ_TOL(bss0.meanEffectivePhyRateMbps.value(),
                              80.0,
                              1e-12,
                              "BSS effective mean is wrong");
    NS_TEST_ASSERT_MSG_EQ_TOL(bss0.aggregateDataTxRateOverIntervalMbps.value(),
                              2.4,
                              1e-12,
                              "BSS interval sum is wrong");
    NS_TEST_ASSERT_MSG_EQ(summary.overall.stations.size(), 5, "Overall stations are not dense");
    NS_TEST_ASSERT_MSG_EQ(summary.overall.accessPoints.size(),
                          3,
                          "Overall BSS values are not dense");
    const auto& idleStation = FindStation(summary.overall.stations, 1, 0).statistics.phyStats;
    NS_TEST_ASSERT_MSG_EQ(idleStation.dataTxRateOverIntervalMbps.value(),
                          0.0,
                          "Idle overall station lacks numeric zero interval rate");
    const auto& idleBss = FindAccessPoint(summary.overall.accessPoints, 1).statistics.phyStats;
    NS_TEST_ASSERT_MSG_EQ(idleBss.meanDominantDataPhyRateMbps.has_value(),
                          false,
                          "All-idle BSS has a defined dominant mean");
    NS_TEST_ASSERT_MSG_EQ(idleBss.aggregateDataTxRateOverIntervalMbps.value(),
                          0.0,
                          "All-idle BSS lacks numeric zero aggregate");
    NS_TEST_ASSERT_MSG_EQ(
        FindStation(summary.overall.stations, 0, 1).statistics.phyStats.dataTxProfile.size(),
        2,
        "Overall profile did not merge distinct window keys");

    auto mismatchedOverall = MergeRawWindows(rawWindows);
    ++mismatchedOverall.at(10).at({20, 1, 9}).ppduAttemptCount;
    const auto mismatch = statistics.BuildSummaryFromRaw(rawWindows, mismatchedOverall);
    NS_TEST_ASSERT_MSG_EQ(mismatch.validation.overallMatchesWindows,
                          false,
                          "Attempt-count mismatch did not fail raw reconstruction");
}

SaturatedTcpBenchmarkLifecycleTestCase::SaturatedTcpBenchmarkLifecycleTestCase()
    : TestCase("connect only ordinary station PHY traces and finalize dense idle output")
{
}

void
SaturatedTcpBenchmarkLifecycleTestCase::DoRun()
{
    auto node = CreateObject<Node>();
    auto device = MakeStationDevice(node);
    SaturatedTcpStatistics statistics(10);
    statistics.RegisterAccessPoint(0, 100, "AP0", "10.1.0.1");
    statistics.RegisterStation(0, 0, node->GetId(), "AP0/STA0", "10.1.0.2");
    statistics.ConnectStation(device);
    statistics.Start(0);
    statistics.Finalize(1'000'000'000);
    const auto summary = statistics.BuildSummary();
    NS_TEST_ASSERT_MSG_EQ(summary.windows.empty(), true, "Idle data-only window was emitted");
    NS_TEST_ASSERT_MSG_EQ(summary.overall.stations.size(), 1, "Idle overall station was lost");
    NS_TEST_ASSERT_MSG_EQ(
        summary.overall.stations.at(0).statistics.phyStats.dataTxRateOverIntervalMbps.value(),
        0.0,
        "Idle connected station lacks numeric zero interval rate");

    bool duplicateRejected = false;
    try
    {
        statistics.ConnectStation(device);
    }
    catch (const std::logic_error&)
    {
        duplicateRejected = true;
    }
    NS_TEST_ASSERT_MSG_EQ(duplicateRejected, true, "Post-start station connection was accepted");

    auto missingPhyNode = CreateObject<Node>();
    auto missingPhy = MakeStationDevice(missingPhyNode, true);
    SaturatedTcpStatistics missingPhyStatistics(10);
    missingPhyStatistics.RegisterStation(0, 0, missingPhyNode->GetId(), "AP0/STA0", "10.1.0.2");
    bool missingPhyRejected = false;
    try
    {
        missingPhyStatistics.ConnectStation(missingPhy);
    }
    catch (const std::invalid_argument&)
    {
        missingPhyRejected = true;
    }
    NS_TEST_ASSERT_MSG_EQ(missingPhyRejected, true, "Station without a PHY was accepted");
    device->Dispose();
    missingPhy->Dispose();
    Simulator::Destroy();
}

namespace
{

/** @ingroup tests Verify schema v2 JSON roles and validation before output ownership. */
class SaturatedTcpBenchmarkJsonTestCase : public TestCase
{
  public:
    SaturatedTcpBenchmarkJsonTestCase()
        : TestCase("write validated schema v2 saturated TCP benchmark JSON")
    {
    }

  private:
    void DoRun() override
    {
        const auto summary = MakeLiteralSummary();
        SaturatedTcpConfig config;
        config.general.runFolder = "run/benchmark-fixture";
        config.benchmark.stationCountPerBss = 1;
        config.benchmark.rssiRange = SaturatedRssiRange::MEDIUM;
        config.benchmark.interferenceMode = SaturatedInterferenceMode::AP_ONLY_COCHANNEL;
        config.benchmark.trafficMode = SaturatedTrafficMode::UL_DL;

        std::ostringstream output;
        WriteSaturatedTcpExperimentJson(output, summary, config);
        const auto document = nlohmann::ordered_json::parse(output.str());
        NS_TEST_ASSERT_MSG_EQ(document.at("schema_version"), 2, "Wrong benchmark schema version");
        const std::vector<std::string> expectedRoots{"schema_version",
                                                     "measurement_semantics",
                                                     "statistics_window_ms",
                                                     "windows",
                                                     "overall",
                                                     "validation",
                                                     "experiment_metadata"};
        NS_TEST_ASSERT_MSG_EQ(GetKeys(document) == expectedRoots,
                              true,
                              "Benchmark root order changed");
        const auto& station = document.at("windows").at(0).at("stations").at(0).at("phy_stats");
        NS_TEST_ASSERT_MSG_EQ(station.at("dominant_data_phy_rate_mbps"),
                              1000.0,
                              "Wrong station dominant rate JSON");
        NS_TEST_ASSERT_MSG_EQ(station.at("data_tx_profile").at(0).at("channel_width_mhz"),
                              80,
                              "Wrong structured profile width");
        NS_TEST_ASSERT_MSG_EQ(station.at("data_tx_profile").at(0).at("nss"),
                              2,
                              "Wrong structured profile NSS");
        NS_TEST_ASSERT_MSG_EQ(station.at("mean_dominant_data_phy_rate_mbps").is_null(),
                              true,
                              "Station contains BSS field");
        const auto& bss = document.at("windows").at(0).at("access_points").at(0).at("phy_stats");
        NS_TEST_ASSERT_MSG_EQ(bss.at("dominant_data_phy_rate_mbps").is_null(),
                              true,
                              "BSS contains station field");
        NS_TEST_ASSERT_MSG_EQ(bss.at("data_tx_profile").empty(), true, "BSS contains profile");
        NS_TEST_ASSERT_MSG_EQ(bss.at("mean_dominant_data_phy_rate_mbps"),
                              1000.0,
                              "Wrong BSS mean JSON");

        auto inconsistent = summary;
        inconsistent.overall.stations.at(0).statistics.phyStats.effectivePhyRateMbps = 81.0;
        bool inconsistentRejected = false;
        try
        {
            std::ostringstream rejected;
            WriteSaturatedTcpExperimentJson(rejected, inconsistent, config);
        }
        catch (const std::invalid_argument&)
        {
            inconsistentRejected = true;
        }
        NS_TEST_ASSERT_MSG_EQ(inconsistentRejected,
                              true,
                              "Material profile formula mismatch was accepted");

        RejectingStreamBuffer buffer;
        std::ostream rejectingOutput(&buffer);
        WriteSaturatedTcpExperimentJson(rejectingOutput, summary, config);
        NS_TEST_ASSERT_MSG_EQ(rejectingOutput.fail(), true, "Rejected stream write was hidden");

        const auto collisionPath = CreateTempDirFilename("llm-saturated-existing-output.json");
        constexpr std::string_view sentinel{"existing benchmark must survive\n"};
        {
            std::ofstream collisionOutput(collisionPath);
            collisionOutput << sentinel;
        }
        bool collisionRejected = false;
        try
        {
            WriteSaturatedTcpExperimentJson(collisionPath, summary, config);
        }
        catch (const std::runtime_error&)
        {
            collisionRejected = true;
        }
        NS_TEST_ASSERT_MSG_EQ(collisionRejected, true, "Existing output was accepted");
        std::ifstream collisionInput(collisionPath);
        const std::string preserved((std::istreambuf_iterator<char>(collisionInput)),
                                    std::istreambuf_iterator<char>());
        NS_TEST_ASSERT_MSG_EQ(preserved, sentinel, "Existing output was replaced");
    }
};

} // namespace

std::vector<TestCase*>
CreateSaturatedTcpBenchmarkOutputTestCases()
{
    return {new SaturatedTcpDataTxDerivationTestCase,
            new SaturatedTcpBenchmarkSummaryTestCase,
            new SaturatedTcpBenchmarkLifecycleTestCase,
            new SaturatedTcpBenchmarkJsonTestCase};
}
