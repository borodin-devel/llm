#include "../examples/experiment-window-output.h"
#include "../examples/scenario-config.h"
#include "../examples/statistics/json/writer.h"
#include "llm-test-suite.h"

#include "ns3/json.hpp"

#include <array>
#include <set>
#include <sstream>
#include <string>
#include <string_view>

using namespace ns3;

namespace
{

SampleDistributionOutput
MakeDistribution(double base)
{
    return {2, base + 1.0, base + 2.0, base + 3.0, base + 4.0};
}

void
PopulateDirectionOutputs(EntityStatisticsOutput& statistics)
{
    auto& general = statistics.generalStats.uplink;
    general.estimatedTransmittedTcpPayloadBytes = 101;
    general.estimatedMatchedTcpPayloadBytes = 102;
    general.matchedPacketCount = 2;
    general.totalTransmissionDurationUs = 103;
    general.averageTransmissionDurationUs = 51.5;
    general.transmissionDurationStandardDeviationUs = 4.5;
    general.minimumTransmissionDurationUs = 47.0;
    general.maximumTransmissionDurationUs = 56.0;
    general.effectiveThroughputMbps = 7.9;
    general.applicationToPhyDelay = MakeDistribution(10.0);

    auto& app = statistics.appStats.uplink;
    app.acceptedSendCount = 2;
    app.acceptedPayloadBytes = 200;
    app.acceptedThroughputMbps = 0.16;
    app.receiveEventCount = 3;
    app.receivedPayloadBytes = 180;
    app.receivedThroughputMbps = 0.144;
    app.dropEventCount = 1;
    app.droppedPayloadBytes = 20;
    app.receiveInterArrivalTime = MakeDistribution(20.0);
    app.agents = {{"agent-\"b", 1, 80, 0.064, 40.0, 1, 20}, {"agent-a", 1, 120, 0.096, 60.0, 0, 0}};
    app.peers = {{7, "10.1.0.1", 1, 80, 0.064, 40.0, 2, 100, 0.08, 55.0, 1, 20},
                 {9, "10.1.0.3", 1, 120, 0.096, 60.0, 1, 80, 0.064, 45.0, 0, 0}};

    auto& tcp = statistics.tcpStats.uplink;
    tcp.connections = {{7, "10.1.0.1", 10000, 2048.0, 4096, MakeDistribution(30.0)},
                       {9, "10.1.0.3", 5000, 1024.0, 2048, MakeDistribution(40.0)}};

    auto& mac = statistics.macStats.uplink;
    mac.estimatedTransmitEventCount = 4;
    mac.estimatedTransmittedTcpPayloadBytes = 300;
    mac.estimatedTransmitThroughputMbps = 0.24;
    mac.estimatedReceiveEventCount = 3;
    mac.estimatedReceivedTcpPayloadBytes = 250;
    mac.estimatedReceiveThroughputMbps = 0.2;
    mac.transmitDropCount = 1;
    mac.transmitDropPacketBytes = 50;
    mac.mpduDropCount = 5;
    mac.mpduDropBytes = 500;
    mac.dataFailureCount = 6;
    mac.finalDataFailureCount = 7;
    mac.mpduDropsByReason = {{4, 2}, {9, 3}};
    mac.peers = {{7, "10.1.0.1", 2, 100, 0.08, 1, 90, 0.072, 2, 200, 3, 4, {{4, 2}}},
                 {9, "10.1.0.3", 2, 200, 0.16, 2, 160, 0.128, 3, 300, 3, 3, {{9, 3}}}};

    auto& phy = statistics.phyStats;
    phy.busyTimeUs = 4500;
    phy.channelUtilizationPercent = 45.0;
    phy.uplink = {600, 500, 8, 900, 10, 2, 2500.0, 54.0, 0.48, {}};
    phy.uplink.peers = {{7, "10.1.0.1", 250, 200, 4, 1, 1000.0, 48.0, 0.2},
                        {9, "10.1.0.3", 350, 300, 6, 1, 1500.0, 58.0, 0.28}};

    statistics.appStats.downlink.acceptedThroughputMbps = 0.0;
    statistics.appStats.downlink.receivedThroughputMbps = 0.0;
    statistics.macStats.downlink.estimatedTransmitThroughputMbps = 0.0;
    statistics.macStats.downlink.estimatedReceiveThroughputMbps = 0.0;
    statistics.phyStats.downlink.throughputMbps = 0.0;
}

AccessPointStatisticsOutput
MakeAccessPointOutput()
{
    AccessPointStatisticsOutput output{0, 7, "AP-\"zero", "10.1.0.1", {}};
    PopulateDirectionOutputs(output.statistics);
    return output;
}

StationStatisticsOutput
MakeStationOutput()
{
    StationStatisticsOutput output{0, 0, 8, "STA-\\zero", "10.1.0.2", {}};
    PopulateDirectionOutputs(output.statistics);
    return output;
}

UnifiedExperimentSummary
MakeLiteralSummary()
{
    UnifiedExperimentSummary summary;
    summary.statisticsWindowMs = 10;
    summary.windows.push_back({42, 420.0, 10.0, {MakeAccessPointOutput()}, {MakeStationOutput()}});
    summary.overall.accessPoints = {MakeAccessPointOutput()};
    summary.overall.stations = {MakeStationOutput()};
    summary.validation = {true, false, true, false, true, false, true, false};
    summary.accessPointInventory = {
        {ExperimentEntityKind::ACCESS_POINT, 0, std::nullopt, 7, "AP-\"zero", "10.1.0.1"}};
    summary.stationInventory = {
        {ExperimentEntityKind::STATION, 0, 0, 8, "STA-\\zero", "10.1.0.2"},
        {ExperimentEntityKind::STATION, 0, std::nullopt, 9, "STA-one", "10.1.0.3"}};
    return summary;
}

/**
 * @ingroup tests
 *
 * Verify the literal final hierarchy, root order, metadata, and fixed entity shape.
 */
class ExperimentHierarchyJsonTestCase : public TestCase
{
  public:
    ExperimentHierarchyJsonTestCase();

  private:
    void DoRun() override;

    template <std::size_t N>
    void AssertExactKeys(const nlohmann::json& object,
                         const std::array<std::string_view, N>& expected,
                         std::string_view description)
    {
        NS_TEST_ASSERT_MSG_EQ(object.is_object(), true, description << " is not an object");
        NS_TEST_ASSERT_MSG_EQ(object.size(), N, description << " has the wrong key count");
        for (const auto key : expected)
        {
            NS_TEST_ASSERT_MSG_EQ(object.contains(key), true, description << " lacks " << key);
        }
    }

    void RejectOldKeys(const nlohmann::json& value, bool insideConfiguration = false)
    {
        static const std::set<std::string> removed{"wifi_windows",
                                                   "wifi_summary",
                                                   "transmission_summary",
                                                   "cross_layer_summary"};
        if (value.is_object())
        {
            for (const auto& [key, child] : value.items())
            {
                NS_TEST_ASSERT_MSG_EQ(removed.contains(key),
                                      false,
                                      "Removed root remains: " << key);
                if (!insideConfiguration)
                {
                    NS_TEST_ASSERT_MSG_NE(key,
                                          "one_second_intervals",
                                          "Fixed-second output field remains");
                }
                RejectOldKeys(child, insideConfiguration || key == "configuration");
            }
        }
        else if (value.is_array())
        {
            for (const auto& child : value)
            {
                RejectOldKeys(child, insideConfiguration);
            }
        }
    }
};

/**
 * @ingroup tests
 *
 * Verify direct serialization preserves 10,000 sparse window records in order.
 */
class ExperimentHierarchyStreamingScaleTestCase : public TestCase
{
  public:
    ExperimentHierarchyStreamingScaleTestCase();

  private:
    void DoRun() override;
};

ExperimentHierarchyJsonTestCase::ExperimentHierarchyJsonTestCase()
    : TestCase("serialize exact final experiment hierarchy")
{
}

void
ExperimentHierarchyJsonTestCase::DoRun()
{
    ScenarioConfig config;
    config.general.traceFile = "traces/quoted-\"trace\\name.json";
    config.general.runFolder.reset();
    std::ostringstream output;
    WriteExperimentHierarchyJson(output, MakeLiteralSummary(), config);
    const std::string text = output.str();
    NS_TEST_ASSERT_MSG_EQ(text.starts_with("{\n  \"schema_version\": 1,"),
                          true,
                          "Root is not two-space formatted");
    NS_TEST_ASSERT_MSG_EQ(text.ends_with("\n}\n"), true, "Document lacks one final newline");
    NS_TEST_ASSERT_MSG_EQ(text.find("\n    \"measurement_semantics\""),
                          std::string::npos,
                          "Root member has the wrong indentation");
    const auto document = nlohmann::json::parse(text);

    constexpr std::array<std::string_view, 7> roots{"schema_version",
                                                    "measurement_semantics",
                                                    "statistics_window_ms",
                                                    "windows",
                                                    "overall",
                                                    "validation",
                                                    "experiment_metadata"};
    AssertExactKeys(document, roots, "root");
    std::size_t prior = 0;
    for (const auto root : roots)
    {
        const auto position = text.find('"' + std::string(root) + '"', prior);
        NS_TEST_ASSERT_MSG_NE(position, std::string::npos, "Missing ordered root " << root);
        NS_TEST_ASSERT_MSG_EQ(position >= prior, true, "Wrong physical root order for " << root);
        prior = position;
    }
    NS_TEST_ASSERT_MSG_EQ(document.at("schema_version"), 1, "Wrong schema version");
    NS_TEST_ASSERT_MSG_EQ(document.at("statistics_window_ms"), 10, "Wrong window width");

    const auto& window = document.at("windows").at(0);
    AssertExactKeys(window,
                    std::array<std::string_view, 5>{"window_index",
                                                    "window_start_ms",
                                                    "window_duration_ms",
                                                    "access_points",
                                                    "stations"},
                    "window");
    NS_TEST_ASSERT_MSG_EQ(window.at("window_index"), 42, "Wrong 64-bit window index");
    const auto& ap = window.at("access_points").at(0);
    AssertExactKeys(ap,
                    std::array<std::string_view, 9>{"access_point_id",
                                                    "node_id",
                                                    "node_label",
                                                    "ipv4",
                                                    "general_stats",
                                                    "app_stats",
                                                    "tcp_stats",
                                                    "mac_stats",
                                                    "phy_stats"},
                    "access point");
    const auto& station = window.at("stations").at(0);
    AssertExactKeys(station,
                    std::array<std::string_view, 10>{"access_point_id",
                                                     "station_index",
                                                     "node_id",
                                                     "node_label",
                                                     "ipv4",
                                                     "general_stats",
                                                     "app_stats",
                                                     "tcp_stats",
                                                     "mac_stats",
                                                     "phy_stats"},
                    "station");

    constexpr std::array<std::string_view, 2> directions{"uplink", "downlink"};
    for (const auto category : {"general_stats", "app_stats", "tcp_stats", "mac_stats"})
    {
        AssertExactKeys(ap.at(category), directions, category);
    }
    AssertExactKeys(ap.at("phy_stats"),
                    std::array<std::string_view, 4>{"busy_time_us",
                                                    "channel_utilization_percent",
                                                    "uplink",
                                                    "downlink"},
                    "phy_stats");

    AssertExactKeys(ap.at("general_stats").at("uplink"),
                    std::array<std::string_view, 10>{"estimated_transmitted_tcp_payload_bytes",
                                                     "estimated_matched_tcp_payload_bytes",
                                                     "matched_packet_count",
                                                     "total_transmission_duration_us",
                                                     "average_transmission_duration_us",
                                                     "transmission_duration_standard_deviation_us",
                                                     "minimum_transmission_duration_us",
                                                     "maximum_transmission_duration_us",
                                                     "effective_throughput_mbps",
                                                     "application_to_phy_delay"},
                    "general direction");
    AssertExactKeys(ap.at("general_stats").at("uplink").at("application_to_phy_delay"),
                    std::array<std::string_view, 5>{"sample_count",
                                                    "average_us",
                                                    "standard_deviation_us",
                                                    "minimum_us",
                                                    "maximum_us"},
                    "sample distribution");
    AssertExactKeys(ap.at("app_stats").at("uplink"),
                    std::array<std::string_view, 11>{"accepted_send_count",
                                                     "accepted_payload_bytes",
                                                     "accepted_throughput_mbps",
                                                     "receive_event_count",
                                                     "received_payload_bytes",
                                                     "received_throughput_mbps",
                                                     "drop_event_count",
                                                     "dropped_payload_bytes",
                                                     "receive_interarrival_time",
                                                     "agents",
                                                     "peers"},
                    "app direction");
    const auto& agents = ap.at("app_stats").at("uplink").at("agents");
    AssertExactKeys(agents.at(0),
                    std::array<std::string_view, 7>{"agent_key",
                                                    "accepted_send_count",
                                                    "accepted_payload_bytes",
                                                    "accepted_throughput_mbps",
                                                    "accepted_bandwidth_share_percent",
                                                    "drop_event_count",
                                                    "dropped_payload_bytes"},
                    "app agent");
    NS_TEST_ASSERT_MSG_EQ(agents.at(0).at("agent_key"),
                          "agent-\"b",
                          "Supplied agent order changed");
    const auto& appPeer = ap.at("app_stats").at("uplink").at("peers").at(0);
    AssertExactKeys(appPeer,
                    std::array<std::string_view, 12>{"peer_node_id",
                                                     "peer_ipv4",
                                                     "accepted_send_count",
                                                     "accepted_payload_bytes",
                                                     "accepted_throughput_mbps",
                                                     "accepted_bandwidth_share_percent",
                                                     "receive_event_count",
                                                     "received_payload_bytes",
                                                     "received_throughput_mbps",
                                                     "received_bandwidth_share_percent",
                                                     "drop_event_count",
                                                     "dropped_payload_bytes"},
                    "app peer");
    AssertExactKeys(ap.at("tcp_stats").at("uplink"),
                    std::array<std::string_view, 1>{"connections"},
                    "tcp direction");
    AssertExactKeys(ap.at("tcp_stats").at("uplink").at("connections").at(0),
                    std::array<std::string_view, 6>{"peer_node_id",
                                                    "peer_ipv4",
                                                    "congestion_window_observation_duration_us",
                                                    "average_congestion_window_bytes",
                                                    "last_congestion_window_bytes",
                                                    "round_trip_time"},
                    "TCP connection");
    AssertExactKeys(ap.at("mac_stats").at("uplink"),
                    std::array<std::string_view, 14>{"estimated_transmit_event_count",
                                                     "estimated_transmitted_tcp_payload_bytes",
                                                     "estimated_transmit_throughput_mbps",
                                                     "estimated_receive_event_count",
                                                     "estimated_received_tcp_payload_bytes",
                                                     "estimated_receive_throughput_mbps",
                                                     "transmit_drop_count",
                                                     "transmit_drop_packet_bytes",
                                                     "mpdu_drop_count",
                                                     "mpdu_drop_bytes",
                                                     "data_failure_count",
                                                     "final_data_failure_count",
                                                     "mpdu_drops_by_reason",
                                                     "peers"},
                    "mac direction");
    const auto& macPeer = ap.at("mac_stats").at("uplink").at("peers").at(0);
    AssertExactKeys(macPeer,
                    std::array<std::string_view, 13>{"peer_node_id",
                                                     "peer_ipv4",
                                                     "estimated_transmit_event_count",
                                                     "estimated_transmitted_tcp_payload_bytes",
                                                     "estimated_transmit_throughput_mbps",
                                                     "estimated_receive_event_count",
                                                     "estimated_received_tcp_payload_bytes",
                                                     "estimated_receive_throughput_mbps",
                                                     "mpdu_drop_count",
                                                     "mpdu_drop_bytes",
                                                     "data_failure_count",
                                                     "final_data_failure_count",
                                                     "mpdu_drops_by_reason"},
                    "MAC peer");
    NS_TEST_ASSERT_MSG_EQ(macPeer.at("mpdu_drop_count"), 2, "Peer MPDU count was lost");
    NS_TEST_ASSERT_MSG_EQ(macPeer.at("mpdu_drop_bytes"), 200, "Peer MPDU bytes were lost");
    NS_TEST_ASSERT_MSG_EQ(macPeer.at("data_failure_count"), 3, "Peer data failures were lost");
    NS_TEST_ASSERT_MSG_EQ(macPeer.at("final_data_failure_count"),
                          4,
                          "Peer final failures were lost");
    NS_TEST_ASSERT_MSG_EQ(macPeer.at("mpdu_drops_by_reason").at(0).at("reason_code"),
                          4,
                          "Peer reason was lost");
    AssertExactKeys(macPeer.at("mpdu_drops_by_reason").at(0),
                    std::array<std::string_view, 2>{"reason_code", "drop_count"},
                    "MAC reason");

    AssertExactKeys(ap.at("phy_stats").at("uplink"),
                    std::array<std::string_view, 10>{"tagged_payload_bytes",
                                                     "unique_tagged_payload_bytes",
                                                     "tagged_mpdu_count",
                                                     "complete_tagged_mpdu_bytes",
                                                     "transmission_attempt_count",
                                                     "retransmission_count",
                                                     "transmission_airtime_us",
                                                     "average_data_rate_mbps",
                                                     "throughput_mbps",
                                                     "peers"},
                    "phy direction");
    AssertExactKeys(ap.at("phy_stats").at("uplink").at("peers").at(0),
                    std::array<std::string_view, 9>{"peer_node_id",
                                                    "peer_ipv4",
                                                    "tagged_payload_bytes",
                                                    "unique_tagged_payload_bytes",
                                                    "transmission_attempt_count",
                                                    "retransmission_count",
                                                    "transmission_airtime_us",
                                                    "average_data_rate_mbps",
                                                    "throughput_mbps"},
                    "PHY peer");
    NS_TEST_ASSERT_MSG_EQ(
        ap.at("general_stats").at("downlink").at("average_transmission_duration_us").is_null(),
        true,
        "Undefined average was not null");
    NS_TEST_ASSERT_MSG_EQ(ap.at("app_stats").at("downlink").at("accepted_throughput_mbps"),
                          0.0,
                          "Known zero throughput was not numeric");

    AssertExactKeys(document.at("overall"),
                    std::array<std::string_view, 2>{"access_points", "stations"},
                    "overall");
    AssertExactKeys(document.at("validation"),
                    std::array<std::string_view, 8>{"entity_inventory_references_valid",
                                                    "app_agent_totals_consistent",
                                                    "app_peer_totals_consistent",
                                                    "mac_peer_totals_consistent",
                                                    "phy_peer_totals_consistent",
                                                    "ap_station_sender_totals_consistent",
                                                    "overall_matches_windows",
                                                    "unique_phy_payload_within_tagged_payload"},
                    "validation");
    const auto& metadata = document.at("experiment_metadata");
    AssertExactKeys(metadata,
                    std::array<std::string_view, 2>{"configuration", "entity_inventory"},
                    "metadata");
    NS_TEST_ASSERT_MSG_EQ(metadata.at("configuration").size(), 8, "Wrong config section count");
    std::size_t fieldCount = 0;
    for (const auto& [section, fields] : metadata.at("configuration").items())
    {
        (void)section;
        fieldCount += fields.size();
    }
    NS_TEST_ASSERT_MSG_EQ(fieldCount, 36, "Wrong effective config field count");
    const auto& inventory = metadata.at("entity_inventory");
    AssertExactKeys(inventory,
                    std::array<std::string_view, 2>{"access_points", "stations"},
                    "inventory");
    AssertExactKeys(
        inventory.at("access_points").at(0),
        std::array<std::string_view, 4>{"access_point_id", "node_id", "node_label", "ipv4"},
        "AP inventory");
    AssertExactKeys(inventory.at("stations").at(0),
                    std::array<std::string_view, 5>{"access_point_id",
                                                    "station_index",
                                                    "node_id",
                                                    "node_label",
                                                    "ipv4"},
                    "STA inventory");
    NS_TEST_ASSERT_MSG_EQ(inventory.at("stations").at(0).at("station_index"),
                          0,
                          "Populated station index changed");
    AssertExactKeys(inventory.at("stations").at(1),
                    std::array<std::string_view, 5>{"access_point_id",
                                                    "station_index",
                                                    "node_id",
                                                    "node_label",
                                                    "ipv4"},
                    "null-index STA inventory");
    NS_TEST_ASSERT_MSG_EQ(inventory.at("stations").at(1).at("station_index").is_null(),
                          true,
                          "Null station index changed");
    NS_TEST_ASSERT_MSG_EQ(document.contains("resolved_paths"), false, "Resolved paths leaked");
    RejectOldKeys(document);
}

ExperimentHierarchyStreamingScaleTestCase::ExperimentHierarchyStreamingScaleTestCase()
    : TestCase("stream 10000 sparse experiment windows in order")
{
}

void
ExperimentHierarchyStreamingScaleTestCase::DoRun()
{
    UnifiedExperimentSummary summary;
    summary.statisticsWindowMs = 10;
    summary.windows.reserve(10000);
    for (uint64_t index = 0; index < 10000; ++index)
    {
        summary.windows.push_back({index, static_cast<double>(index * 10), 10.0, {}, {}});
    }
    std::ostringstream output;
    WriteExperimentHierarchyJson(output, summary, {});
    const auto document = nlohmann::json::parse(output.str());
    const auto& windows = document.at("windows");
    NS_TEST_ASSERT_MSG_EQ(windows.size(), 10000, "Streaming scale lost windows");
    for (uint64_t index = 0; index < windows.size(); ++index)
    {
        NS_TEST_ASSERT_MSG_EQ(windows.at(index).at("window_index"),
                              index,
                              "Streaming scale reordered windows");
    }
}

} // namespace

std::vector<TestCase*>
CreateExperimentHierarchyJsonTestCases()
{
    return {new ExperimentHierarchyJsonTestCase, new ExperimentHierarchyStreamingScaleTestCase};
}
