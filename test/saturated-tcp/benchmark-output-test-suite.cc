#include "../../examples/saturated-tcp/access-tracking-sta-wifi-mac.h"
#include "../../examples/saturated-tcp/access-wait-tracker.h"
#include "../../examples/saturated-tcp/benchmark-statistics.h"
#include "../../examples/saturated-tcp/config.h"
#include "../../examples/statistics/json/writer.h"
#include "../llm-test-suite.h"

#include "ns3/boolean.h"
#include "ns3/channel-access-manager.h"
#include "ns3/enum.h"
#include "ns3/json.hpp"
#include "ns3/node.h"
#include "ns3/pointer.h"
#include "ns3/qos-txop.h"
#include "ns3/simulator.h"
#include "ns3/wifi-net-device.h"
#include "ns3/yans-wifi-phy.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
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

using RawStationWindows = std::map<uint32_t, std::vector<StationPhyMetricAccumulator>>;

/** Stream buffer that rejects every body write. */
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

/**
 * Set literal raw metrics for one complete 10 ms window.
 *
 * @param windows Raw station windows.
 * @param nodeId Station node identifier.
 * @param windowIndex Window index to populate.
 * @param theoreticalMbps Desired theoretical rate, or a negative value for no PPDU.
 * @param practicalMbps Desired practical rate, ignored when no PPDU is requested.
 * @param contentionNs Literal contention duration in nanoseconds.
 */
void
SetRawWindow(RawStationWindows& windows,
             uint32_t nodeId,
             std::size_t windowIndex,
             double theoreticalMbps,
             double practicalMbps,
             int64_t contentionNs)
{
    auto& raw = windows.at(nodeId).at(windowIndex);
    if (theoreticalMbps >= 0.0)
    {
        constexpr int64_t airtimeNs = 1'000'000;
        raw.nominalRateBpsNs = static_cast<long double>(theoreticalMbps) * 1'000'000.0L * airtimeNs;
        raw.psduBits = static_cast<long double>(practicalMbps) * airtimeNs / 1000.0L;
        raw.ppduAirtimeNs = airtimeNs;
    }
    raw.contentionNs = contentionNs;
}

/**
 * Find an access point output by BSS identifier.
 *
 * @param accessPoints Access point outputs.
 * @param accessPointId Desired BSS identifier.
 * @return Matching output.
 */
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

/**
 * Find a station output by BSS and station index.
 *
 * @param stations Station outputs.
 * @param accessPointId Parent BSS identifier.
 * @param stationIndex Desired station index.
 * @return Matching output.
 */
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

/**
 * Set the four benchmark PHY fields.
 *
 * @param statistics Entity statistics to populate.
 * @param theoreticalMbps Theoretical PHY rate.
 * @param practicalMbps Practical PHY rate.
 * @param contentionFraction Contention fraction.
 */
void
SetBenchmarkPhy(EntityStatisticsOutput& statistics,
                double theoreticalMbps,
                double practicalMbps,
                double contentionFraction)
{
    auto& phy = statistics.phyStats;
    phy.averageTheoreticalPhyRateMbps = theoreticalMbps;
    phy.averagePracticalPhyRateMbps = practicalMbps;
    phy.channelEfficiency = practicalMbps / theoreticalMbps;
    phy.contentionFraction = contentionFraction;
}

/**
 * Construct one literal benchmark summary with one sparse window.
 *
 * @return Complete three-BSS summary.
 */
UnifiedExperimentSummary
MakeLiteralSummary()
{
    UnifiedExperimentSummary summary;
    summary.statisticsWindowMs = 10;

    ExperimentWindowOutput window;
    window.windowIndex = 0;
    window.windowStartMs = 0.0;
    window.windowDurationMs = 10.0;
    for (uint32_t accessPointId = 0; accessPointId < 3; ++accessPointId)
    {
        const uint32_t accessPointNodeId = 100 + accessPointId;
        const uint32_t stationNodeId = 200 + accessPointId;
        const double theoreticalMbps = 100.0 * (accessPointId + 1);
        const double practicalMbps = theoreticalMbps / 2.0;
        const double contentionFraction = 0.1 * (accessPointId + 1);

        ExperimentEntityIdentity accessPointIdentity{ExperimentEntityKind::ACCESS_POINT,
                                                     accessPointId,
                                                     std::nullopt,
                                                     accessPointNodeId,
                                                     "AP" + std::to_string(accessPointId),
                                                     "10.1." + std::to_string(accessPointId) +
                                                         ".1"};
        ExperimentEntityIdentity stationIdentity{ExperimentEntityKind::STATION,
                                                 accessPointId,
                                                 0,
                                                 stationNodeId,
                                                 "AP" + std::to_string(accessPointId) + "/STA0",
                                                 "10.1." + std::to_string(accessPointId) + ".2"};
        summary.accessPointInventory.push_back(accessPointIdentity);
        summary.stationInventory.push_back(stationIdentity);

        AccessPointStatisticsOutput accessPoint{accessPointId,
                                                accessPointNodeId,
                                                accessPointIdentity.nodeLabel,
                                                accessPointIdentity.ipv4,
                                                {}};
        StationStatisticsOutput station{accessPointId,
                                        0,
                                        stationNodeId,
                                        stationIdentity.nodeLabel,
                                        stationIdentity.ipv4,
                                        {}};
        SetBenchmarkPhy(accessPoint.statistics, theoreticalMbps, practicalMbps, contentionFraction);
        SetBenchmarkPhy(station.statistics, theoreticalMbps, practicalMbps, contentionFraction);
        window.accessPoints.push_back(accessPoint);
        window.stations.push_back(station);
        summary.overall.accessPoints.push_back(accessPoint);
        summary.overall.stations.push_back(station);
    }
    summary.windows.push_back(window);
    return summary;
}

/**
 * Get object keys in their serialized order.
 *
 * @param object Ordered JSON object.
 * @return Ordered member names.
 */
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

/**
 * Construct a minimally connected benchmark station device.
 *
 * @param node Device owner.
 * @param address Station MAC address.
 * @return Wi-Fi device with the benchmark station MAC, QoS TXOPs, and one PHY.
 */
Ptr<WifiNetDevice>
MakeStationDevice(Ptr<Node> node, Mac48Address address)
{
    const auto makeQosTxop = [](AcIndex ac) {
        return CreateObjectWithAttributes<QosTxop>("AcIndex", EnumValue(ac));
    };
    auto mac =
        CreateObjectWithAttributes<AccessTrackingStaWifiMac>("QosSupported",
                                                             BooleanValue(true),
                                                             "BE_Txop",
                                                             PointerValue(makeQosTxop(AC_BE)),
                                                             "BK_Txop",
                                                             PointerValue(makeQosTxop(AC_BK)),
                                                             "VI_Txop",
                                                             PointerValue(makeQosTxop(AC_VI)),
                                                             "VO_Txop",
                                                             PointerValue(makeQosTxop(AC_VO)));
    mac->SetAddress(address);
    mac->SetChannelAccessManagers({CreateObject<ChannelAccessManager>()});
    for (const auto ac : {AC_BE, AC_BK, AC_VI, AC_VO})
    {
        mac->GetQosTxop(ac)->SetWifiMac(mac);
    }

    auto device = CreateObject<WifiNetDevice>();
    device->SetMac(mac);
    device->SetPhy(CreateObject<YansWifiPhy>());
    node->AddDevice(device);
    return device;
}

} // namespace

/**
 * @ingroup tests
 *
 * Verify station DTO derivation, BSS means, sparse windows, and dense overall values.
 */
class SaturatedTcpBenchmarkSummaryTestCase : public TestCase
{
  public:
    SaturatedTcpBenchmarkSummaryTestCase();

  private:
    void DoRun() override;
};

SaturatedTcpBenchmarkSummaryTestCase::SaturatedTcpBenchmarkSummaryTestCase()
    : TestCase("build station-only saturated TCP BSS summaries")
{
}

void
SaturatedTcpBenchmarkSummaryTestCase::DoRun()
{
    SaturatedTcpStatistics statistics(10);
    statistics.RegisterAccessPoint(2, 102, "AP2", "10.1.2.1");
    statistics.RegisterAccessPoint(0, 100, "AP0", "10.1.0.1");
    statistics.RegisterAccessPoint(1, 101, "AP1", "10.1.1.1");
    statistics.RegisterStation(0, 1, 11, "AP0/STA1", "10.1.0.3");
    statistics.RegisterStation(2, 0, 30, "AP2/STA0", "10.1.2.2");
    statistics.RegisterStation(0, 0, 10, "AP0/STA0", "10.1.0.2");
    statistics.RegisterStation(1, 0, 20, "AP1/STA0", "10.1.1.2");

    RawStationWindows rawWindows;
    for (const uint32_t nodeId : {10, 11, 20, 30, 100})
    {
        rawWindows.emplace(nodeId, std::vector<StationPhyMetricAccumulator>(100));
    }
    SetRawWindow(rawWindows, 10, 0, 100.0, 80.0, 1'000'000);
    SetRawWindow(rawWindows, 11, 0, -1.0, 0.0, 2'000'000);
    SetRawWindow(rawWindows, 11, 1, 300.0, 150.0, 0);
    SetRawWindow(rawWindows, 20, 0, 200.0, 100.0, 0);
    SetRawWindow(rawWindows, 30, 0, 400.0, 200.0, 5'000'000);
    SetRawWindow(rawWindows, 100, 0, 900.0, 900.0, 9'000'000);

    const auto summary = statistics.BuildSummaryFromRaw(rawWindows);
    NS_TEST_ASSERT_MSG_EQ(summary.statisticsWindowMs, 10, "Wrong statistics window width");
    NS_TEST_ASSERT_MSG_EQ(summary.windows.size(), 2, "Inactive windows were not sparse");
    NS_TEST_ASSERT_MSG_EQ(summary.windows.at(0).windowIndex, 0, "Wrong first window index");
    NS_TEST_ASSERT_MSG_EQ(summary.windows.at(1).windowIndex, 1, "Wrong second window index");
    NS_TEST_ASSERT_MSG_EQ(summary.windows.at(0).stations.size(),
                          4,
                          "Active station window entries were lost");
    NS_TEST_ASSERT_MSG_EQ(summary.windows.at(1).stations.size(),
                          1,
                          "Inactive station was emitted in the second window");

    const auto& station0 = FindStation(summary.windows.at(0).stations, 0, 0);
    NS_TEST_ASSERT_MSG_EQ_TOL(station0.statistics.phyStats.averageTheoreticalPhyRateMbps.value(),
                              100.0,
                              1e-12,
                              "Station theoretical rate was not copied from the derived value");
    NS_TEST_ASSERT_MSG_EQ_TOL(station0.statistics.phyStats.averagePracticalPhyRateMbps.value(),
                              80.0,
                              1e-12,
                              "Station practical rate was not copied from the derived value");
    NS_TEST_ASSERT_MSG_EQ_TOL(station0.statistics.phyStats.channelEfficiency.value(),
                              0.8,
                              1e-12,
                              "Station efficiency was not copied from the derived value");
    NS_TEST_ASSERT_MSG_EQ_TOL(station0.statistics.phyStats.contentionFraction.value(),
                              0.1,
                              1e-12,
                              "Station contention was not copied from the derived value");

    const auto& station1 = FindStation(summary.windows.at(0).stations, 0, 1);
    NS_TEST_ASSERT_MSG_EQ(station1.statistics.phyStats.averageTheoreticalPhyRateMbps.has_value(),
                          false,
                          "No-PPDU station window has a theoretical rate");
    NS_TEST_ASSERT_MSG_EQ(station1.statistics.phyStats.averagePracticalPhyRateMbps.has_value(),
                          false,
                          "No-PPDU station window has a practical rate");
    NS_TEST_ASSERT_MSG_EQ(station1.statistics.phyStats.channelEfficiency.has_value(),
                          false,
                          "No-PPDU station window has an efficiency");
    NS_TEST_ASSERT_MSG_EQ_TOL(station1.statistics.phyStats.contentionFraction.value(),
                              0.2,
                              1e-12,
                              "Contention-only station window was not retained");

    const auto& accessPoint0 = FindAccessPoint(summary.windows.at(0).accessPoints, 0);
    NS_TEST_ASSERT_MSG_EQ_TOL(
        accessPoint0.statistics.phyStats.averageTheoreticalPhyRateMbps.value(),
        100.0,
        1e-12,
        "AP theoretical mean included a null station rate or AP-originated raw data");
    NS_TEST_ASSERT_MSG_EQ_TOL(
        accessPoint0.statistics.phyStats.averagePracticalPhyRateMbps.value(),
        80.0,
        1e-12,
        "AP practical mean included a null station rate or AP-originated raw data");
    NS_TEST_ASSERT_MSG_EQ_TOL(accessPoint0.statistics.phyStats.channelEfficiency.value(),
                              0.8,
                              1e-12,
                              "AP efficiency is not practical divided by theoretical");
    NS_TEST_ASSERT_MSG_EQ_TOL(accessPoint0.statistics.phyStats.contentionFraction.value(),
                              0.15,
                              1e-12,
                              "AP contention is not the mean of every child station");

    NS_TEST_ASSERT_MSG_EQ(summary.overall.accessPoints.size(), 3, "Overall AP output is not dense");
    NS_TEST_ASSERT_MSG_EQ(summary.overall.stations.size(),
                          4,
                          "Overall station output is not dense");
    const auto& overallAccessPoint0 = FindAccessPoint(summary.overall.accessPoints, 0);
    NS_TEST_ASSERT_MSG_EQ_TOL(
        overallAccessPoint0.statistics.phyStats.averageTheoreticalPhyRateMbps.value(),
        200.0,
        1e-12,
        "Overall AP theoretical rate is not the station arithmetic mean");
    NS_TEST_ASSERT_MSG_EQ_TOL(
        overallAccessPoint0.statistics.phyStats.averagePracticalPhyRateMbps.value(),
        115.0,
        1e-12,
        "Overall AP practical rate is not the station arithmetic mean");
    NS_TEST_ASSERT_MSG_EQ_TOL(overallAccessPoint0.statistics.phyStats.channelEfficiency.value(),
                              0.575,
                              1e-12,
                              "Overall AP efficiency is not the ratio of AP rates");
    NS_TEST_ASSERT_MSG_EQ_TOL(overallAccessPoint0.statistics.phyStats.contentionFraction.value(),
                              0.0015,
                              1e-12,
                              "Overall AP contention is not the station arithmetic mean");

    NS_TEST_ASSERT_MSG_EQ(summary.validation.entityInventoryReferencesValid,
                          true,
                          "Valid station parents failed inventory validation");
    NS_TEST_ASSERT_MSG_EQ(summary.validation.overallMatchesWindows,
                          true,
                          "Raw overall did not match merged windows");

    SaturatedTcpStatistics invalidInventory(10);
    invalidInventory.RegisterAccessPoint(0, 500, "AP0", "10.2.0.1");
    invalidInventory.RegisterStation(9, 0, 501, "AP9/STA0", "10.2.9.2");
    RawStationWindows invalidRaw{{501, std::vector<StationPhyMetricAccumulator>(100)}};
    SetRawWindow(invalidRaw, 501, 0, 100.0, 50.0, 0);
    const auto invalidSummary = invalidInventory.BuildSummaryFromRaw(invalidRaw);
    NS_TEST_ASSERT_MSG_EQ(invalidSummary.validation.entityInventoryReferencesValid,
                          false,
                          "Missing station parent passed inventory validation");
}

/**
 * @ingroup tests
 *
 * Verify station-only trace connection and exact, idempotent measurement lifecycle handling.
 */
class SaturatedTcpBenchmarkLifecycleTestCase : public TestCase
{
  public:
    SaturatedTcpBenchmarkLifecycleTestCase();

  private:
    void DoRun() override;
};

SaturatedTcpBenchmarkLifecycleTestCase::SaturatedTcpBenchmarkLifecycleTestCase()
    : TestCase("connect and finalize saturated TCP station statistics")
{
}

void
SaturatedTcpBenchmarkLifecycleTestCase::DoRun()
{
    auto accessPointNode = CreateObject<Node>();
    auto stationNode = CreateObject<Node>();
    auto accessPointDevice = MakeStationDevice(accessPointNode, Mac48Address("00:00:00:00:00:01"));
    auto stationDevice = MakeStationDevice(stationNode, Mac48Address("00:00:00:00:00:02"));

    SaturatedTcpStatistics statistics(10);
    statistics.RegisterAccessPoint(0, accessPointNode->GetId(), "AP0", "10.1.0.1");
    statistics.RegisterStation(0, 0, stationNode->GetId(), "AP0/STA0", "10.1.0.2");
    try
    {
        statistics.ConnectStation(accessPointDevice);
        NS_TEST_ASSERT_MSG_EQ(true, false, "Registered AP device was connected as a station");
    }
    catch (const std::invalid_argument&)
    {
    }
    statistics.ConnectStation(stationDevice);
    try
    {
        statistics.ConnectStation(stationDevice);
        NS_TEST_ASSERT_MSG_EQ(true, false, "Station device was connected twice");
    }
    catch (const std::invalid_argument&)
    {
    }

    const uint32_t stationNodeId = stationNode->GetId();
    statistics.NotifyAccessRequested(stationNodeId, AC_BE, 0);
    statistics.Start(0);
    try
    {
        statistics.Start(0);
        NS_TEST_ASSERT_MSG_EQ(true, false, "Statistics measurement started twice");
    }
    catch (const std::logic_error&)
    {
    }

    statistics.NotifyAccessRequested(stationNodeId, AC_BE, 0);
    statistics.NotifyTxopGranted(stationNodeId, AC_BE, MilliSeconds(2), MilliSeconds(1), 0);
    statistics.m_accessWaitTrackers.at(stationNodeId)
        ->NotifyRequest(AC_BE, 0, MilliSeconds(5).GetNanoSeconds());

    try
    {
        statistics.Finalize(999'999'999);
        NS_TEST_ASSERT_MSG_EQ(true, false, "Non-one-second endpoint was accepted");
    }
    catch (const std::invalid_argument&)
    {
    }
    statistics.Finalize(1'000'000'000);
    statistics.Finalize(1'000'000'000);
    const auto finalizedRaw = statistics.m_phyRecorder->BuildOverallAccumulator(stationNodeId);
    NS_TEST_ASSERT_MSG_EQ(finalizedRaw.contentionNs,
                          997'000'000,
                          "Pre-start callback contributed or pending wait closed at the wrong end");

    statistics.NotifyAccessRequested(stationNodeId, AC_BE, 0);
    statistics.Finalize(1'000'000'000);
    NS_TEST_ASSERT_MSG_EQ(
        statistics.m_phyRecorder->BuildOverallAccumulator(stationNodeId).contentionNs,
        finalizedRaw.contentionNs,
        "Post-finalization callback or repeated finalization changed raw state");

    accessPointDevice->Dispose();
    stationDevice->Dispose();
    Simulator::Destroy();
}

namespace
{

/**
 * @ingroup tests
 *
 * Verify benchmark JSON sections, hierarchy values, and output lifecycle.
 */
class SaturatedTcpBenchmarkJsonTestCase : public TestCase
{
  public:
    SaturatedTcpBenchmarkJsonTestCase();

  private:
    void DoRun() override;
    void CheckKeys(const nlohmann::ordered_json& object,
                   const std::vector<std::string>& expected,
                   std::string_view objectName);
    void CheckPathFailure(const std::filesystem::path& outputPath,
                          const UnifiedExperimentSummary& summary,
                          const SaturatedTcpConfig& config,
                          std::string_view description);
};

SaturatedTcpBenchmarkJsonTestCase::SaturatedTcpBenchmarkJsonTestCase()
    : TestCase("write shared saturated TCP benchmark JSON exclusively")
{
}

void
SaturatedTcpBenchmarkJsonTestCase::CheckKeys(const nlohmann::ordered_json& object,
                                             const std::vector<std::string>& expected,
                                             std::string_view objectName)
{
    const auto actual = GetKeys(object);
    NS_TEST_ASSERT_MSG_EQ(actual.size(), expected.size(), "Wrong key count in " << objectName);
    if (actual.size() != expected.size())
    {
        return;
    }
    for (std::size_t index = 0; index < expected.size(); ++index)
    {
        NS_TEST_ASSERT_MSG_EQ(actual.at(index),
                              expected.at(index),
                              "Wrong key at index " << index << " in " << objectName);
    }
}

void
SaturatedTcpBenchmarkJsonTestCase::CheckPathFailure(const std::filesystem::path& outputPath,
                                                    const UnifiedExperimentSummary& summary,
                                                    const SaturatedTcpConfig& config,
                                                    std::string_view description)
{
    try
    {
        WriteSaturatedTcpExperimentJson(outputPath.string(), summary, config);
        NS_TEST_ASSERT_MSG_EQ(true, false, "Writer failure was ignored: " << description);
    }
    catch (const std::runtime_error& error)
    {
        NS_TEST_ASSERT_MSG_NE(std::string(error.what()).find(outputPath.string()),
                              std::string::npos,
                              "Writer error lacks output path for " << description << ": "
                                                                    << error.what());
    }
}

void
SaturatedTcpBenchmarkJsonTestCase::DoRun()
{
    const auto summary = MakeLiteralSummary();
    SaturatedTcpConfig config;
    config.general.runFolder = "run/benchmark-fixture";
    config.benchmark.stationCountPerBss = 1;
    config.benchmark.rssiRange = SaturatedRssiRange::MEDIUM;
    config.benchmark.interferenceMode = SaturatedInterferenceMode::AP_ONLY_COCHANNEL;
    config.benchmark.trafficMode = SaturatedTrafficMode::UL_DL;

    const std::filesystem::path outputPath =
        CreateTempDirFilename("llm-saturated-benchmark-output.json");
    WriteSaturatedTcpExperimentJson(outputPath.string(), summary, config);
    std::ifstream input(outputPath);
    const std::string text((std::istreambuf_iterator<char>(input)),
                           std::istreambuf_iterator<char>());
    NS_TEST_ASSERT_MSG_EQ(text.ends_with("\n"), true, "Benchmark JSON lacks a final newline");
    NS_TEST_ASSERT_MSG_EQ(text.ends_with("\n\n"), false, "Benchmark JSON has extra newlines");

    const auto document = nlohmann::ordered_json::parse(text);
    const std::vector<std::string> expectedRoots{"schema_version",
                                                 "measurement_semantics",
                                                 "statistics_window_ms",
                                                 "windows",
                                                 "overall",
                                                 "validation",
                                                 "experiment_metadata"};
    CheckKeys(document, expectedRoots, "benchmark root");
    NS_TEST_ASSERT_MSG_EQ(document.at("schema_version"), 1, "Benchmark schema version changed");
    NS_TEST_ASSERT_MSG_EQ(document.at("measurement_semantics").at("phy_observation_scope"),
                          "qualifying station-transmitted PPDUs",
                          "Benchmark semantics do not identify station-transmitted PPDUs");
    NS_TEST_ASSERT_MSG_EQ(
        document.at("measurement_semantics").at("undefined_derived_values").is_null(),
        true,
        "Undefined benchmark derived values are not JSON null");

    const auto& configuration = document.at("experiment_metadata").at("configuration");
    const std::vector<std::string> expectedSections{"general",
                                                    "script",
                                                    "simulation",
                                                    "benchmark",
                                                    "wifi",
                                                    "tcp",
                                                    "statistics",
                                                    "logging"};
    CheckKeys(configuration, expectedSections, "benchmark configuration");
    NS_TEST_ASSERT_MSG_EQ(configuration.at("benchmark").at("sta_count_per_bss"),
                          1,
                          "Wrong station-count metadata");
    NS_TEST_ASSERT_MSG_EQ(configuration.at("benchmark").at("rssi_range"),
                          "medium",
                          "Wrong RSSI metadata");
    NS_TEST_ASSERT_MSG_EQ(configuration.at("benchmark").at("interference_mode"),
                          "ap_only_cochannel",
                          "Wrong interference metadata");
    NS_TEST_ASSERT_MSG_EQ(configuration.at("benchmark").at("traffic_mode"),
                          "ul_dl",
                          "Wrong traffic metadata");

    const auto& station = document.at("windows").at(0).at("stations").at(0);
    const auto& stationPhy = station.at("phy_stats");
    NS_TEST_ASSERT_MSG_EQ(stationPhy.at("average_theoretical_phy_rate_mbps"),
                          100.0,
                          "Wrong station theoretical JSON field");
    NS_TEST_ASSERT_MSG_EQ(stationPhy.at("average_practical_phy_rate_mbps"),
                          50.0,
                          "Wrong station practical JSON field");
    NS_TEST_ASSERT_MSG_EQ(stationPhy.at("channel_efficiency"),
                          0.5,
                          "Wrong station efficiency JSON field");
    NS_TEST_ASSERT_MSG_EQ(stationPhy.at("contention_fraction"),
                          0.1,
                          "Wrong station contention JSON field");
    const auto& accessPointPhy =
        document.at("windows").at(0).at("access_points").at(0).at("phy_stats");
    NS_TEST_ASSERT_MSG_EQ(accessPointPhy,
                          stationPhy,
                          "One-station AP fields are not exact station-derived values");

    const std::vector<std::string> expectedEntityKeys{"access_point_id",
                                                      "station_index",
                                                      "node_id",
                                                      "node_label",
                                                      "ipv4",
                                                      "general_stats",
                                                      "app_stats",
                                                      "tcp_stats",
                                                      "mac_stats",
                                                      "phy_stats"};
    CheckKeys(station, expectedEntityKeys, "station entity");
    for (const auto category : {"general_stats", "app_stats", "tcp_stats", "mac_stats"})
    {
        CheckKeys(station.at(category), {"uplink", "downlink"}, category);
    }
    NS_TEST_ASSERT_MSG_EQ(
        station.at("general_stats").at("uplink").at("estimated_transmitted_tcp_payload_bytes"),
        0,
        "Benchmark populated unrelated general statistics");
    NS_TEST_ASSERT_MSG_EQ(station.at("app_stats").at("uplink").at("agents").empty(),
                          true,
                          "Benchmark populated unrelated application statistics");
    NS_TEST_ASSERT_MSG_EQ(station.at("tcp_stats").at("uplink").at("connections").empty(),
                          true,
                          "Benchmark populated unrelated TCP statistics");
    NS_TEST_ASSERT_MSG_EQ(station.at("mac_stats").at("uplink").at("peers").empty(),
                          true,
                          "Benchmark populated unrelated MAC statistics");

    const auto& validation = document.at("validation");
    constexpr std::array validationKeys{
        "entity_inventory_references_valid",
        "app_agent_totals_consistent",
        "app_peer_totals_consistent",
        "mac_peer_totals_consistent",
        "phy_peer_totals_consistent",
        "ap_station_sender_totals_consistent",
        "overall_matches_windows",
        "unique_phy_payload_within_tagged_payload",
    };
    NS_TEST_ASSERT_MSG_EQ(validation.size(), 8, "Benchmark validation shape changed");
    for (const auto key : validationKeys)
    {
        NS_TEST_ASSERT_MSG_EQ(validation.at(key), true, "Validation flag is false: " << key);
    }

    RejectingStreamBuffer buffer;
    std::ostream rejectingOutput(&buffer);
    WriteSaturatedTcpExperimentJson(rejectingOutput, summary, config);
    NS_TEST_ASSERT_MSG_EQ(rejectingOutput.fail(),
                          true,
                          "Rejected benchmark hierarchy write was not observable");

    const std::filesystem::path collisionPath =
        CreateTempDirFilename("llm-saturated-existing-output.json");
    constexpr std::string_view sentinel{"existing benchmark must survive\n"};
    {
        std::ofstream collisionOutput(collisionPath);
        collisionOutput << sentinel;
    }
    CheckPathFailure(collisionPath, summary, config, "existing output collision");
    std::ifstream collisionInput(collisionPath);
    const std::string preserved((std::istreambuf_iterator<char>(collisionInput)),
                                std::istreambuf_iterator<char>());
    NS_TEST_ASSERT_MSG_EQ(preserved,
                          sentinel,
                          "Existing benchmark output was replaced on collision");

    const std::filesystem::path missingParentPath =
        std::filesystem::path(CreateTempDirFilename("missing-saturated-parent")) / "output.json";
    NS_TEST_ASSERT_MSG_EQ(std::filesystem::exists(missingParentPath.parent_path()),
                          false,
                          "Missing-parent fixture unexpectedly exists");
    CheckPathFailure(missingParentPath, summary, config, "missing output parent");
    NS_TEST_ASSERT_MSG_EQ(std::filesystem::exists(missingParentPath),
                          false,
                          "Failed benchmark writer created an output file");

    auto inconsistent = summary;
    inconsistent.overall.accessPoints.at(0).statistics.phyStats.averagePracticalPhyRateMbps = 51.0;
    const std::filesystem::path invalidPath =
        CreateTempDirFilename("llm-saturated-invalid-output.json");
    try
    {
        WriteSaturatedTcpExperimentJson(invalidPath.string(), inconsistent, config);
        NS_TEST_ASSERT_MSG_EQ(true, false, "Inconsistent AP summary was written");
    }
    catch (const std::invalid_argument&)
    {
    }
    NS_TEST_ASSERT_MSG_EQ(std::filesystem::exists(invalidPath),
                          false,
                          "Invalid summary created an output file before rejection");
}

} // namespace

std::vector<TestCase*>
CreateSaturatedTcpBenchmarkOutputTestCases()
{
    return {new SaturatedTcpBenchmarkSummaryTestCase,
            new SaturatedTcpBenchmarkLifecycleTestCase,
            new SaturatedTcpBenchmarkJsonTestCase};
}
