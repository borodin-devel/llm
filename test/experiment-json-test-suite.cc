#include "../examples/experiment-output-internal.h"
#include "../examples/scenario-config.h"
#include "../examples/traffic-coordinator.h"
#include "../examples/wifi-statistics-internal.h"
#include "../examples/wifi-statistics.h"
#include "llm-test-suite.h"

#include "ns3/json.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace ns3;

/**
 * @ingroup tests
 *
 * Verify the complete experiment document schema and representative values.
 */
class ExperimentJsonTestCase : public TestCase
{
  public:
    ExperimentJsonTestCase();

  private:
    void DoRun() override;
    void AssertDelay(const nlohmann::json& delay, const DelaySummary& expected);
};

namespace
{

/**
 * Collect every object key in a JSON value.
 *
 * @param value JSON value to visit.
 * @param keys Destination key set.
 */
void
CollectObjectKeys(const nlohmann::json& value, std::set<std::string>& keys)
{
    if (value.is_object())
    {
        for (const auto& [key, child] : value.items())
        {
            keys.insert(key);
            CollectObjectKeys(child, keys);
        }
    }
    else if (value.is_array())
    {
        for (const auto& child : value)
        {
            CollectObjectKeys(child, keys);
        }
    }
}

/**
 * @ingroup tests
 *
 * Verify sparse Wi-Fi window indexes remain 64-bit in the new schema.
 */
class LongDurationExperimentJsonTestCase : public TestCase
{
  public:
    LongDurationExperimentJsonTestCase();

  private:
    void DoRun() override;
};

/**
 * @ingroup tests
 *
 * Verify experiment writer failures preserve existing output.
 */
class ExperimentJsonFailureTestCase : public TestCase
{
  public:
    ExperimentJsonFailureTestCase();

  private:
    void DoRun() override;
    void CheckWriteFailure(const WifiStatistics& statistics,
                           const std::filesystem::path& outputPath,
                           std::string_view description);
};

LongDurationExperimentJsonTestCase::LongDurationExperimentJsonTestCase()
    : TestCase("preserve 64-bit experiment JSON windows beyond 2^32 milliseconds")
{
}

ExperimentJsonFailureTestCase::ExperimentJsonFailureTestCase()
    : TestCase("report experiment JSON output failures without clobbering")
{
}

void
LongDurationExperimentJsonTestCase::DoRun()
{
    constexpr uint64_t firstIndexBeyond32Bits = uint64_t{1} << 32;
    constexpr uint64_t durationMs = firstIndexBeyond32Bits + 1;
    constexpr int64_t bucketStartUs = static_cast<int64_t>(firstIndexBeyond32Bits * 1000);

    uint64_t windowIndex = 0;
    NS_TEST_ASSERT_MSG_EQ(
        GetStatisticsWindowIndex(bucketStartUs, 0, static_cast<double>(durationMs), 1, windowIndex),
        true,
        "Long-duration window was rejected");
    NS_TEST_ASSERT_MSG_EQ(windowIndex,
                          firstIndexBeyond32Bits,
                          "Statistics window index wrapped at 32 bits");

    TrafficCoordinator coordinator(static_cast<double>(durationMs),
                                   static_cast<double>(durationMs));
    WifiStatisticsState statistics(coordinator, 1);
    statistics.stationIpsByBss = {{"10.1.0.2"}};
    statistics.phyWindows[windowIndex][0].upBytes["10.1.0.2"] = 1;

    std::ostringstream output;
    output << '{';
    (void)WriteWifiStatisticsJsonMembers(output, statistics);
    output << '}';
    const auto document = nlohmann::json::parse(output.str());
    NS_TEST_ASSERT_MSG_EQ(document.at("wifi_windows").size(),
                          1,
                          "64-bit sparse statistics bucket was not emitted");
    NS_TEST_ASSERT_MSG_EQ(document.at("wifi_windows").at(0).at("window_end_ms").get<uint64_t>(),
                          durationMs,
                          "64-bit statistics timestamp wrapped");
}

void
ExperimentJsonFailureTestCase::CheckWriteFailure(const WifiStatistics& statistics,
                                                 const std::filesystem::path& outputPath,
                                                 std::string_view description)
{
    try
    {
        statistics.WriteExperimentJson(outputPath.string(), {}, {}, {});
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
ExperimentJsonFailureTestCase::DoRun()
{
    TrafficCoordinator coordinator(25.0, 25.0);
    WifiStatistics statistics(coordinator, 25);

    const std::filesystem::path collisionPath =
        CreateTempDirFilename("llm-experiment-existing.json");
    constexpr std::string_view sentinel{"existing experiment must survive\n"};
    {
        std::ofstream output(collisionPath);
        output << sentinel;
    }

    CheckWriteFailure(statistics, collisionPath, "existing output collision");

    std::ifstream collisionInput(collisionPath);
    const std::string preserved((std::istreambuf_iterator<char>(collisionInput)),
                                std::istreambuf_iterator<char>());
    NS_TEST_ASSERT_MSG_EQ(preserved,
                          sentinel,
                          "Existing output content was replaced by the experiment writer");

    const std::filesystem::path missingParentPath =
        std::filesystem::path(CreateTempDirFilename("missing-experiment-parent")) / "output.json";
    NS_TEST_ASSERT_MSG_EQ(std::filesystem::exists(missingParentPath.parent_path()),
                          false,
                          "Missing-parent fixture unexpectedly exists");
    CheckWriteFailure(statistics, missingParentPath, "missing output parent");
    NS_TEST_ASSERT_MSG_EQ(std::filesystem::exists(missingParentPath),
                          false,
                          "Failed writer created an output file");
}

} // namespace

ExperimentJsonTestCase::ExperimentJsonTestCase()
    : TestCase("serialize complete experiment JSON with precise schema")
{
}

void
ExperimentJsonTestCase::AssertDelay(const nlohmann::json& delay, const DelaySummary& expected)
{
    NS_TEST_ASSERT_MSG_EQ(delay.at("sample_count"), expected.sampleCount, "Wrong delay count");
    NS_TEST_ASSERT_MSG_EQ(delay.at("mean_us"), expected.meanUs, "Wrong delay mean");
    NS_TEST_ASSERT_MSG_EQ(delay.at("standard_deviation_us"),
                          expected.standardDeviationUs,
                          "Wrong delay deviation");
    NS_TEST_ASSERT_MSG_EQ(delay.at("minimum_us"), expected.minimumUs, "Wrong delay minimum");
    NS_TEST_ASSERT_MSG_EQ(delay.at("maximum_us"), expected.maximumUs, "Wrong delay maximum");
}

void
ExperimentJsonTestCase::DoRun()
{
    TrafficCoordinator coordinator(100.0, 100.0);
    WifiStatistics statistics(coordinator, 25);
    auto& state = *statistics.m_state;
    state.stationIpsByBss = {{"10.1.0.2", "10.1.0.3"}};
    auto& uplinkWindow = state.phyWindows[0][0];
    uplinkWindow.upBytes["10.1.0.2"] = 1000;
    uplinkWindow.upPhyRates["10.1.0.2"].Add(12e6, 100.0);
    state.phyWindows[3][0].downBytes["10.1.0.3"] = 80;

    TransmissionSummary transmissionSummary;
    transmissionSummary.senders.push_back({
        .senderIpv4 = "10.1.0.2",
        .matchedPacketCount = 2,
        .totalTransmissionDurationUs = 500,
        .transmittedPayloadBytes = 3000,
        .effectiveThroughputMbps = 48.0,
    });

    const DelaySummary intervalDelay{3, 20.0, 5.0, 10.0, 30.0};
    CrossLayerIntervalSummary interval;
    interval.intervalIndex = 4;
    interval.intervalStartS = 4.0;
    interval.intervalDurationS = 0.75;
    interval.applicationToPhyDelay = intervalDelay;
    interval.applicationTransmitThroughputMbps = 1.25;
    interval.phyPayloadThroughputMbps = 2.5;
    interval.uniquePhyPayloadThroughputMbps = 2.0;
    interval.channelUtilizationPercent = 33.5;
    interval.phyRetransmissionCount = 4;
    interval.macTransmitDropCount = 5;
    interval.macTransmitDropBytes = 600;
    interval.macMpduDropCount = 7;
    interval.macMpduDropBytes = 800;
    interval.macDataFailureCount = 9;
    interval.macFinalDataFailureCount = 10;
    interval.applicationDropEventCount = 11;
    interval.applicationDropBytes = 1200;
    interval.macMpduDropsByReason = {{13, 14}};
    interval.applicationDropsByAgent = {{"agent-\"quoted\\path", 15, 1600}};

    const DelaySummary overallDelay{6, 40.0, 7.0, 8.0, 90.0};
    CrossLayerOverallSummary overall;
    overall.experimentDurationS = 5.25;
    overall.applicationToPhyDelay = overallDelay;
    overall.applicationTransmittedPayloadBytes = 1700;
    overall.phyPayloadBytes = 1800;
    overall.uniquePhyPayloadBytes = 1900;
    overall.phyMpduBytes = 2000;
    overall.averageApplicationTransmitThroughputMbps = 2.25;
    overall.averagePhyPayloadThroughputMbps = 3.5;
    overall.averageChannelUtilizationPercent = 44.5;
    overall.phyRetransmissionCount = 21;
    overall.macTransmitDropCount = 22;
    overall.macTransmitDropBytes = 2300;
    overall.macMpduDropCount = 24;
    overall.macMpduDropBytes = 2500;
    overall.macDataFailureCount = 26;
    overall.macFinalDataFailureCount = 27;
    overall.applicationDropEventCount = 28;
    overall.applicationDropBytes = 2900;
    overall.macMpduDropsByReason = {{30, 31}};

    CrossLayerSummary crossLayerSummary;
    crossLayerSummary.nodes.push_back({
        .nodeId = 32,
        .nodeLabel = "AP-\"quoted\\label",
        .oneSecondIntervals = {interval},
        .overall = overall,
    });

    ScenarioConfig configuration;
    configuration.general.traceFile =
        "traces/quoted-\"name\\folder\u0434\u0430\u043d\u043d\u044b\u0435.json";
    configuration.general.runFolder.reset();
    configuration.general.outputName = "custom-output.json";
    configuration.simulation.durationMode = DurationMode::FIXED;
    configuration.simulation.fixedDurationSeconds = 12.5;
    configuration.topology.isolateBssChannels = false;
    configuration.wifi.band = WifiBandConfig::BAND_6_GHZ;
    configuration.wifi.activeProbing = false;
    configuration.statistics.windowMs = 25;
    configuration.logging.sampleScenarioLevel = "debug";

    const std::string outputPath = CreateTempDirFilename("llm-experiment.json");
    statistics.WriteExperimentJson(outputPath,
                                   transmissionSummary,
                                   crossLayerSummary,
                                   configuration);

    std::ifstream input(outputPath);
    NS_TEST_ASSERT_MSG_EQ(input.good(), true, "Experiment JSON was not created");
    const nlohmann::json document = nlohmann::json::parse(input);

    NS_TEST_ASSERT_MSG_EQ(document.size(), 9, "Wrong root member count");
    NS_TEST_ASSERT_MSG_EQ(document.at("schema_version"), 1, "Wrong schema version");
    NS_TEST_ASSERT_MSG_EQ(document.contains("measurement_semantics"), true, "Missing semantics");
    NS_TEST_ASSERT_MSG_EQ(document.at("statistics_window_ms"), 25, "Wrong window width");
    NS_TEST_ASSERT_MSG_EQ(document.contains("wifi_windows"), true, "Missing Wi-Fi windows");
    NS_TEST_ASSERT_MSG_EQ(document.contains("wifi_summary"), true, "Missing Wi-Fi summary");
    NS_TEST_ASSERT_MSG_EQ(document.contains("transmission_summary"),
                          true,
                          "Missing transmission summary");
    NS_TEST_ASSERT_MSG_EQ(document.contains("cross_layer_summary"),
                          true,
                          "Missing cross-layer summary");
    NS_TEST_ASSERT_MSG_EQ(document.contains("validation"), true, "Missing validation");
    NS_TEST_ASSERT_MSG_EQ(document.contains("experiment_metadata"), true, "Missing metadata");

    const auto& firstWindow = document.at("wifi_windows").at(0);
    NS_TEST_ASSERT_MSG_EQ(firstWindow.at("window_end_ms"), 25, "Wrong first window end");
    const auto& accessPoint = firstWindow.at("access_points").at(0);
    NS_TEST_ASSERT_MSG_EQ(accessPoint.at("access_point_id"), 0, "Wrong access point ID");
    NS_TEST_ASSERT_MSG_EQ(accessPoint.at("uplink").at("total_payload_bytes"),
                          1000,
                          "Wrong uplink window total");
    const auto& uplinkFlow = accessPoint.at("uplink").at("flows").at(0);
    NS_TEST_ASSERT_MSG_EQ(uplinkFlow.at("station_ipv4"), "10.1.0.2", "Wrong station address");
    NS_TEST_ASSERT_MSG_EQ(uplinkFlow.at("payload_bytes"), 1000, "Wrong window payload bytes");
    NS_TEST_ASSERT_MSG_EQ(uplinkFlow.at("throughput_mbps"), 0.32, "Wrong window throughput");
    NS_TEST_ASSERT_MSG_EQ(uplinkFlow.at("average_phy_data_rate_mbps"),
                          12.0,
                          "Wrong average PHY rate");
    NS_TEST_ASSERT_MSG_EQ(uplinkFlow.at("phy_transmission_attempt_count"),
                          1,
                          "Wrong PHY attempt count");
    NS_TEST_ASSERT_MSG_EQ(uplinkFlow.at("phy_transmission_airtime_us"), 100.0, "Wrong PHY airtime");
    NS_TEST_ASSERT_MSG_EQ(accessPoint.at("downlink").at("total_payload_bytes"),
                          0,
                          "Wrong first-window downlink total");

    const auto& wifiSummary = document.at("wifi_summary").at(0);
    NS_TEST_ASSERT_MSG_EQ(wifiSummary.at("access_point_id"), 0, "Wrong summary AP ID");
    NS_TEST_ASSERT_MSG_EQ(wifiSummary.at("uplink").at("total_payload_bytes"),
                          1000,
                          "Wrong summary uplink total");
    const auto& summaryFlow = wifiSummary.at("uplink").at("flows").at(0);
    NS_TEST_ASSERT_MSG_EQ(summaryFlow.at("station_ipv4"),
                          "10.1.0.2",
                          "Wrong summary station address");
    NS_TEST_ASSERT_MSG_EQ(summaryFlow.at("total_payload_bytes"), 1000, "Wrong summary flow total");
    NS_TEST_ASSERT_MSG_EQ(summaryFlow.at("average_phy_data_rate_mbps"),
                          12.0,
                          "Wrong summary PHY rate");
    NS_TEST_ASSERT_MSG_EQ(summaryFlow.at("phy_transmission_attempt_count"),
                          1,
                          "Wrong summary attempt count");
    NS_TEST_ASSERT_MSG_EQ(summaryFlow.at("phy_transmission_airtime_us"),
                          100.0,
                          "Wrong summary airtime");
    NS_TEST_ASSERT_MSG_EQ(wifiSummary.at("downlink").at("total_payload_bytes"),
                          80,
                          "Wrong summary downlink total");

    const auto& sender = document.at("transmission_summary").at("senders").at(0);
    NS_TEST_ASSERT_MSG_EQ(sender.at("sender_ipv4"), "10.1.0.2", "Wrong sender address");
    NS_TEST_ASSERT_MSG_EQ(sender.at("matched_packet_count"), 2, "Wrong matched packet count");
    NS_TEST_ASSERT_MSG_EQ(sender.at("total_transmission_duration_us"), 500, "Wrong duration");
    NS_TEST_ASSERT_MSG_EQ(sender.at("transmitted_payload_bytes"), 3000, "Wrong sender bytes");
    NS_TEST_ASSERT_MSG_EQ(sender.at("effective_throughput_mbps"), 48.0, "Wrong sender rate");

    const auto& node = document.at("cross_layer_summary").at("nodes").at(0);
    NS_TEST_ASSERT_MSG_EQ(node.at("node_id"), 32, "Wrong node ID");
    NS_TEST_ASSERT_MSG_EQ(node.at("node_label"), "AP-\"quoted\\label", "Wrong node label");
    const auto& serializedInterval = node.at("one_second_intervals").at(0);
    NS_TEST_ASSERT_MSG_EQ(serializedInterval.at("interval_index"), 4, "Wrong interval index");
    NS_TEST_ASSERT_MSG_EQ(serializedInterval.at("interval_start_s"), 4.0, "Wrong interval start");
    NS_TEST_ASSERT_MSG_EQ(serializedInterval.at("interval_duration_s"),
                          0.75,
                          "Wrong interval duration");
    AssertDelay(serializedInterval.at("application_to_phy_delay"), intervalDelay);
    NS_TEST_ASSERT_MSG_EQ(serializedInterval.at("application_transmit_throughput_mbps"),
                          1.25,
                          "Wrong application rate");
    NS_TEST_ASSERT_MSG_EQ(serializedInterval.at("phy_payload_throughput_mbps"),
                          2.5,
                          "Wrong PHY payload rate");
    NS_TEST_ASSERT_MSG_EQ(serializedInterval.at("unique_phy_payload_throughput_mbps"),
                          2.0,
                          "Wrong unique PHY rate");
    NS_TEST_ASSERT_MSG_EQ(serializedInterval.at("channel_utilization_percent"),
                          33.5,
                          "Wrong utilization");
    NS_TEST_ASSERT_MSG_EQ(serializedInterval.at("phy_retransmission_count"),
                          4,
                          "Wrong retransmission count");
    NS_TEST_ASSERT_MSG_EQ(serializedInterval.at("mac_transmit_drop_count"), 5, "Wrong TX drops");
    NS_TEST_ASSERT_MSG_EQ(serializedInterval.at("mac_transmit_drop_bytes"), 600, "Wrong TX bytes");
    NS_TEST_ASSERT_MSG_EQ(serializedInterval.at("mac_mpdu_drop_count"), 7, "Wrong MPDU drops");
    NS_TEST_ASSERT_MSG_EQ(serializedInterval.at("mac_mpdu_drop_bytes"), 800, "Wrong MPDU bytes");
    NS_TEST_ASSERT_MSG_EQ(serializedInterval.at("mac_data_failure_count"),
                          9,
                          "Wrong data failures");
    NS_TEST_ASSERT_MSG_EQ(serializedInterval.at("mac_final_data_failure_count"),
                          10,
                          "Wrong final failures");
    NS_TEST_ASSERT_MSG_EQ(serializedInterval.at("application_drop_event_count"),
                          11,
                          "Wrong app drops");
    NS_TEST_ASSERT_MSG_EQ(serializedInterval.at("application_drop_bytes"),
                          1200,
                          "Wrong app drop bytes");
    const auto& intervalReason = serializedInterval.at("mac_mpdu_drops_by_reason").at(0);
    NS_TEST_ASSERT_MSG_EQ(intervalReason.at("reason_code"), 13, "Wrong reason code");
    NS_TEST_ASSERT_MSG_EQ(intervalReason.at("drop_count"), 14, "Wrong reason count");
    const auto& agent = serializedInterval.at("application_drops_by_agent").at(0);
    NS_TEST_ASSERT_MSG_EQ(agent.at("agent_key"), "agent-\"quoted\\path", "Wrong escaped agent key");
    NS_TEST_ASSERT_MSG_EQ(agent.at("drop_event_count"), 15, "Wrong agent drop count");
    NS_TEST_ASSERT_MSG_EQ(agent.at("dropped_payload_bytes"), 1600, "Wrong agent drop bytes");

    const auto& serializedOverall = node.at("overall");
    NS_TEST_ASSERT_MSG_EQ(serializedOverall.at("experiment_duration_s"),
                          5.25,
                          "Wrong experiment duration");
    AssertDelay(serializedOverall.at("application_to_phy_delay"), overallDelay);
    NS_TEST_ASSERT_MSG_EQ(serializedOverall.at("application_transmitted_payload_bytes"),
                          1700,
                          "Wrong overall app bytes");
    NS_TEST_ASSERT_MSG_EQ(serializedOverall.at("phy_payload_bytes"), 1800, "Wrong PHY bytes");
    NS_TEST_ASSERT_MSG_EQ(serializedOverall.at("unique_phy_payload_bytes"),
                          1900,
                          "Wrong unique bytes");
    NS_TEST_ASSERT_MSG_EQ(serializedOverall.at("phy_mpdu_bytes"), 2000, "Wrong MPDU bytes");
    NS_TEST_ASSERT_MSG_EQ(serializedOverall.at("average_application_transmit_throughput_mbps"),
                          2.25,
                          "Wrong average app rate");
    NS_TEST_ASSERT_MSG_EQ(serializedOverall.at("average_phy_payload_throughput_mbps"),
                          3.5,
                          "Wrong average PHY rate");
    NS_TEST_ASSERT_MSG_EQ(serializedOverall.at("average_channel_utilization_percent"),
                          44.5,
                          "Wrong average utilization");
    NS_TEST_ASSERT_MSG_EQ(serializedOverall.at("phy_retransmission_count"),
                          21,
                          "Wrong overall retransmissions");
    NS_TEST_ASSERT_MSG_EQ(serializedOverall.at("mac_transmit_drop_count"), 22, "Wrong TX drops");
    NS_TEST_ASSERT_MSG_EQ(serializedOverall.at("mac_transmit_drop_bytes"), 2300, "Wrong TX bytes");
    NS_TEST_ASSERT_MSG_EQ(serializedOverall.at("mac_mpdu_drop_count"), 24, "Wrong MPDU drops");
    NS_TEST_ASSERT_MSG_EQ(serializedOverall.at("mac_mpdu_drop_bytes"), 2500, "Wrong MPDU bytes");
    NS_TEST_ASSERT_MSG_EQ(serializedOverall.at("mac_data_failure_count"),
                          26,
                          "Wrong data failures");
    NS_TEST_ASSERT_MSG_EQ(serializedOverall.at("mac_final_data_failure_count"),
                          27,
                          "Wrong final failures");
    NS_TEST_ASSERT_MSG_EQ(serializedOverall.at("application_drop_event_count"),
                          28,
                          "Wrong app drops");
    NS_TEST_ASSERT_MSG_EQ(serializedOverall.at("application_drop_bytes"), 2900, "Wrong app bytes");
    const auto& overallReason = serializedOverall.at("mac_mpdu_drops_by_reason").at(0);
    NS_TEST_ASSERT_MSG_EQ(overallReason.at("reason_code"), 30, "Wrong overall reason");
    NS_TEST_ASSERT_MSG_EQ(overallReason.at("drop_count"), 31, "Wrong overall reason count");

    const auto& validation = document.at("validation");
    NS_TEST_ASSERT_MSG_EQ(validation.at("window_payload_totals_consistent"),
                          true,
                          "Window validation failed");
    NS_TEST_ASSERT_MSG_EQ(validation.at("summary_payload_totals_consistent"),
                          true,
                          "Summary validation failed");

    const auto& metadata = document.at("experiment_metadata");
    const auto& serializedConfiguration = metadata.at("configuration");
    NS_TEST_ASSERT_MSG_EQ(serializedConfiguration.size(), 8, "Wrong configuration section count");
    std::size_t configurationFieldCount = 0;
    for (const auto& section : serializedConfiguration.items())
    {
        configurationFieldCount += section.value().size();
    }
    NS_TEST_ASSERT_MSG_EQ(configurationFieldCount, 36, "Wrong effective configuration field count");
    NS_TEST_ASSERT_MSG_EQ(serializedConfiguration.at("general").at("trace_file"),
                          configuration.general.traceFile,
                          "Wrong escaped trace path");
    NS_TEST_ASSERT_MSG_EQ(serializedConfiguration.at("general").at("run_folder").is_null(),
                          true,
                          "Omitted run folder is not null");
    NS_TEST_ASSERT_MSG_EQ(serializedConfiguration.at("simulation").at("duration_mode"),
                          "fixed",
                          "Wrong duration mode");
    NS_TEST_ASSERT_MSG_EQ(serializedConfiguration.at("simulation").at("fixed_duration_seconds"),
                          12.5,
                          "Wrong configured duration");
    NS_TEST_ASSERT_MSG_EQ(serializedConfiguration.at("topology").at("isolate_bss_channels"),
                          false,
                          "Wrong configured channel isolation");
    NS_TEST_ASSERT_MSG_EQ(serializedConfiguration.at("wifi").at("band"),
                          "6GHz",
                          "Wrong configured band");
    NS_TEST_ASSERT_MSG_EQ(serializedConfiguration.at("statistics").at("window_ms"),
                          25,
                          "Wrong configured statistics window");
    NS_TEST_ASSERT_MSG_EQ(document.contains("resolved_paths"), false, "Resolved paths leaked");

    std::set<std::string> objectKeys;
    for (const auto& [key, value] : document.items())
    {
        objectKeys.insert(key);
        if (key == "experiment_metadata")
        {
            for (const auto& [metadataKey, metadataValue] : value.items())
            {
                if (metadataKey != "configuration")
                {
                    objectKeys.insert(metadataKey);
                    CollectObjectKeys(metadataValue, objectKeys);
                }
            }
        }
        else
        {
            CollectObjectKeys(value, objectKeys);
        }
    }
    constexpr std::array<std::string_view, 20> removedKeys{
        "source",
        "byte_semantics",
        "phy_rate_semantics",
        "window_ms",
        "windows",
        "summary",
        "timestamp",
        "stats",
        "ap_id",
        "up_flows",
        "down_flows",
        "up_total_bytes",
        "down_total_bytes",
        "host_id",
        "bytes",
        "bw",
        "avg_phy_data_rate_mbps",
        "phy_tx_attempts",
        "phy_tx_airtime_us",
        "window_totals_consistent",
    };
    for (const auto key : removedKeys)
    {
        NS_TEST_ASSERT_MSG_EQ(objectKeys.contains(std::string(key)), false, "Removed key " << key);
    }
    NS_TEST_ASSERT_MSG_EQ(objectKeys.contains("summary_totals_consistent"),
                          false,
                          "Removed summary validation key remains");

    std::ifstream rawInput(outputPath);
    const std::string raw((std::istreambuf_iterator<char>(rawInput)),
                          std::istreambuf_iterator<char>());
    constexpr std::array<std::string_view, 9> rootOrder{"schema_version",
                                                        "measurement_semantics",
                                                        "statistics_window_ms",
                                                        "wifi_windows",
                                                        "wifi_summary",
                                                        "transmission_summary",
                                                        "cross_layer_summary",
                                                        "validation",
                                                        "experiment_metadata"};
    std::size_t previousPosition = 0;
    for (const auto key : rootOrder)
    {
        const std::size_t position = raw.find('"' + std::string(key) + '"', previousPosition);
        NS_TEST_ASSERT_MSG_NE(position, std::string::npos, "Missing ordered root key " << key);
        NS_TEST_ASSERT_MSG_EQ(position >= previousPosition, true, "Wrong root order for " << key);
        previousPosition = position;
    }
    NS_TEST_ASSERT_MSG_EQ(raw.find("\"validation\"") < raw.find("\"experiment_metadata\""),
                          true,
                          "Metadata was not emitted after validation");
}

std::vector<TestCase*>
CreateExperimentJsonTestCases()
{
    return {new ExperimentJsonTestCase,
            new LongDurationExperimentJsonTestCase,
            new ExperimentJsonFailureTestCase};
}
