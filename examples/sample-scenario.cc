// examples/sample-scenario.cc
// Sample ns-3 scenario: 3 APs x 3 stations, 802.11ax, TCP, separate YansWifiChannel per AP group
//
// Usage: ./sample-scenario <traces.json> [bandwidth_mhz] [stats_output.json] [experiment_time]
//   bandwidth_mhz: 20, 40, 80 or 160 (default: 20)
//   stats_output.json: MAC per-node statistics (default: mac-node-stats.json)
//   experiment_time: "auto" (JSON duration + 2s, default) or fixed seconds (> 0)
//

#include "scenario-log.h"
#include "traffic-coordinator.h"
#include "wifi-statistics.h"

#include "ns3/ap-generator.h"
#include "ns3/sta-llm-generator.h"
#include "ns3/traffic-sink.h"
#include "ns3/agent-distribution.h"
#include "ns3/contention-aware-agent-distribution.h"

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/mobility-module.h"
#include "ns3/wifi-module.h"
#include "ns3/applications-module.h"
#include "ns3/error-rate-model.h"
#include "ns3/yans-wifi-helper.h"

#include "ns3/wifi-mac-header.h"
#include "ns3/wifi-mpdu.h"
#include "ns3/wifi-phy.h"
#include "ns3/wifi-psdu.h"
#include "ns3/wifi-tx-vector.h"
#include "ns3/wifi-phy-state-helper.h"
#include "ns3/wifi-remote-station-manager.h"
#include "ns3/ipv4-header.h"
#include "ns3/tcp-header.h"

#include <iostream>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <tuple>
#include <string>
#include <vector>
#include <map>
#include <functional>
#include <fstream>
#include <iomanip>
#include <stdexcept>
#include <limits>
#include <set>
#include <sstream>

using namespace ns3;


// Agent map key: "id_type" (e.g. "1_GUI交互综合Agent")
using AgentMap = std::map<std::string, std::vector<Operation>>;

static std::string
Ipv4ToString(Ipv4Address address)
{
    std::ostringstream stream;
    address.Print(stream);
    return stream.str();
}

static LogComponent& g_log = llm_example::GetScenarioLog();

// ============================================================================
// Device TX/RX trace maps
// ============================================================================

struct TxRxKey
{
    std::string txSrcIp;
    uint16_t txSrcPort;
    std::string rxSrcIp;
    uint16_t rxSrcPort;
    uint32_t bytes;

    bool operator==(const TxRxKey& other) const
    {
        return txSrcIp == other.txSrcIp &&
               txSrcPort == other.txSrcPort &&
               rxSrcIp == other.rxSrcIp &&
               rxSrcPort == other.rxSrcPort &&
               bytes == other.bytes;
    }
};

struct TxRxKeyHash
{
    std::size_t operator()(const TxRxKey& k) const
    {
        std::size_t h1 = std::hash<std::string>{}(k.txSrcIp);
        std::size_t h2 = std::hash<uint16_t>{}(k.txSrcPort);
        std::size_t h3 = std::hash<std::string>{}(k.rxSrcIp);
        std::size_t h4 = std::hash<uint16_t>{}(k.rxSrcPort);
        std::size_t h5 = std::hash<uint32_t>{}(k.bytes);
        return h1 ^ (h2 << 1) ^ (h3 << 2) ^ (h4 << 3) ^ (h5 << 4);
    }
};

std::map<std::string, std::vector<uint64_t>> g_txBytes;

std::map<TxRxKey, std::vector<uint64_t>, std::function<bool(const TxRxKey&, const TxRxKey&)>> g_txMap(
    [](const TxRxKey& a, const TxRxKey& b) { return std::make_tuple(a.txSrcIp, a.txSrcPort, a.rxSrcIp, a.rxSrcPort, a.bytes) <
                                                      std::make_tuple(b.txSrcIp, b.txSrcPort, b.rxSrcIp, b.rxSrcPort, b.bytes); });

std::map<TxRxKey, std::vector<uint64_t>, std::function<bool(const TxRxKey&, const TxRxKey&)>> g_rxMap(
    [](const TxRxKey& a, const TxRxKey& b) { return std::make_tuple(a.txSrcIp, a.txSrcPort, a.rxSrcIp, a.rxSrcPort, a.bytes) <
                                                      std::make_tuple(b.txSrcIp, b.txSrcPort, b.rxSrcIp, b.rxSrcPort, b.bytes); });

// ============================================================================
// Fixed-window MAC/PHY statistics
// ============================================================================

static constexpr double kAutoExperimentTailMarginMs = 2000.0;

// Free trace callbacks are moved into owners in the next extraction tasks.
static const TrafficCoordinator* g_trafficCoordinator = nullptr;
static WifiStatistics* g_wifiStatistics = nullptr;
void PrintTransmissionTimePerSender()
{
    NS_LOG_INFO("========== MAC Layer Transmission time per sender ==========");

    std::map<std::string, uint64_t> senderTotalDiffUs;
    std::map<std::string, uint64_t> senderTotalBytes;

    for (const auto& [key, rxTimestamps] : g_rxMap)
    {
        auto txIt = g_txMap.find(key);
        if (txIt == g_txMap.end() || txIt->second.empty())
        {
            continue;
        }

        const auto& txTimestamps = txIt->second;
        std::size_t minSize = std::min(txTimestamps.size(), rxTimestamps.size());

        for (std::size_t i = 0; i < minSize; ++i)
        {
            int64_t diff = static_cast<int64_t>(rxTimestamps[i]) - static_cast<int64_t>(txTimestamps[i]);
            if (diff > 0)
            {
                senderTotalDiffUs[key.txSrcIp] += static_cast<uint64_t>(diff);
            }
        }
    }

    for (const auto& [k, byteArr] : g_txBytes)
    {
        int sum = std::accumulate(byteArr.begin(), byteArr.end(), 0);
        senderTotalBytes[k] = sum;
    }

    for (const auto& [sender, totalUs] : senderTotalDiffUs)
    {
        double totalMs = static_cast<double>(totalUs) / 1000.0;
        double totalSec = static_cast<double>(totalUs) / 1e6;
        double totalBytesMb = static_cast<double>(senderTotalBytes[sender]) / (1024.0 * 1024.0);
        NS_LOG_INFO("Sender " << sender <<
            ": txTime=" << totalMs << " ms (" << totalSec << " s), " <<
            "PayloadOnly=" << senderTotalBytes[sender] << " (" << totalBytesMb << " MB)" <<
            "effRate=" << totalBytesMb * 8 / totalSec << " mbps");
    }

    NS_LOG_INFO("============================================================");
}

void DeviceTxTrace (std::string context, Ptr<const Packet> packet)
{
    if (g_trafficCoordinator->GetExperimentStartUs() < 0 ||
        Simulator::Now().GetMicroSeconds() < g_trafficCoordinator->GetExperimentStartUs())
    {
        return;
    }
    Ptr<Packet> pktCopy = packet->Copy ();
    LlcSnapHeader llc;
    pktCopy->RemoveHeader (llc);

    const uint32_t packetSize = packet->GetSize ();
    if (packetSize <= 60)
    {
        return;
    }
    const uint32_t payloadSize = packetSize - 60;

    Ipv4Header ipHeader;
    std::string srcIp = "Unknown";
    std::string dstIp = "Unknown";

    if (pktCopy->PeekHeader (ipHeader))
    {
        std::ostringstream srcStream, dstStream;
        ipHeader.GetSource ().Print (srcStream);
        ipHeader.GetDestination ().Print (dstStream);
        srcIp = srcStream.str ();
        dstIp = dstStream.str ();

        pktCopy->RemoveHeader (ipHeader);
    }
    else
    {
        return;
    }

    TcpHeader tcpHeader;
    uint16_t srcPort = 0;
    uint16_t dstPort = 0;
    bool isTcp = pktCopy->PeekHeader (tcpHeader);

    if (isTcp)
    {
        srcPort = tcpHeader.GetSourcePort ();
        dstPort = tcpHeader.GetDestinationPort ();
    }
    else
    {
        return;
    }

    NS_LOG_INFO ("TX [" << Simulator::Now ().GetMicroSeconds () << " us] "
                 << "PayloadOnly: " << payloadSize << " | "
                 << "tx: " << srcIp << ":" << srcPort << " -> "
                 << "rx: " << dstIp << ":" << dstPort);

    g_wifiStatistics->RecordMacPayload(Simulator::Now().GetMicroSeconds(),
                                       srcIp,
                                       dstIp,
                                       payloadSize);

    TxRxKey key{srcIp, srcPort, dstIp, dstPort, payloadSize};
    g_txMap[key].push_back(Simulator::Now().GetMicroSeconds());
    g_txBytes[srcIp].push_back(payloadSize);
}

void DeviceRxTrace (std::string context, Ptr<const Packet> packet)
{
    if (g_trafficCoordinator->GetExperimentStartUs() < 0 ||
        Simulator::Now().GetMicroSeconds() < g_trafficCoordinator->GetExperimentStartUs())
    {
        return;
    }
    Ptr<Packet> pktCopy = packet->Copy ();
    LlcSnapHeader llc;
    pktCopy->RemoveHeader (llc);

    const uint32_t packetSize = packet->GetSize ();
    if (packetSize <= 60)
    {
        return;
    }
    const uint32_t payloadSize = packetSize - 60;

    Ipv4Header ipHeader;
    std::string srcIp = "Unknown";
    std::string dstIp = "Unknown";

    if (pktCopy->PeekHeader (ipHeader))
    {
        std::ostringstream srcStream, dstStream;
        ipHeader.GetSource ().Print (srcStream);
        ipHeader.GetDestination ().Print (dstStream);
        srcIp = srcStream.str ();
        dstIp = dstStream.str ();

        pktCopy->RemoveHeader (ipHeader);
    }
    else
    {
        return;
    }

    TcpHeader tcpHeader;
    uint16_t srcPort = 0;
    uint16_t dstPort = 0;
    bool isTcp = pktCopy->PeekHeader (tcpHeader);

    if (isTcp)
    {
        srcPort = tcpHeader.GetSourcePort ();
        dstPort = tcpHeader.GetDestinationPort ();
    }
    else
    {
        return;
    }

    NS_LOG_INFO ("RX [" << Simulator::Now ().GetMicroSeconds() << " us] "
                 << "Payload: " << payloadSize << " | "
                 << "tx: " << srcIp << ":" << srcPort << " -> "
                 << "rx: " << dstIp << ":" << dstPort);
    TxRxKey key{srcIp, srcPort, dstIp, dstPort, payloadSize};
    g_rxMap[key].push_back(Simulator::Now().GetMicroSeconds());
}

// ============================================================================
// Wi-Fi association diagnostics
// ============================================================================

static void
StaAssociated(int apIndex, uint32_t staIndex, Mac48Address bssid)
{
    NS_LOG_INFO("[Wi-Fi association] AP group " << apIndex
                << " STA " << staIndex
                << " associated with BSSID " << bssid);
}

// ============================================================================
// Helper: set up one AP + its stations
// ============================================================================

static void
SetupApGroup(int apIndex,
             int bandwidthMhz,
             const std::map<std::string, Address>& agentStationMap,
             const AgentMap& agentMap,
             Address apAddress,
             uint32_t staNum,
             TrafficCoordinator& trafficCoordinator,
             WifiStatistics& wifiStatistics)
{
    NS_LOG_INFO("=== Setting up AP group " << apIndex
                << ", BW " << bandwidthMhz << " MHz ===");

    // Create nodes
    NodeContainer apNode;
    apNode.Create(1); // AP node

    NodeContainer stationNodes;
    stationNodes.Create(staNum); // stations assigned to this AP

    // Create a physically isolated YansWifiChannel for this AP group.
    // PHYs belonging to different AP groups cannot hear or interfere with
    // one another, regardless of their coordinates or configured frequency.
    YansWifiChannelHelper channelHelper = YansWifiChannelHelper::Default();
    Ptr<YansWifiChannel> channel = channelHelper.Create();

    // The channel number intentionally remains 0: this hard model represents
    // physical separation through independent channel objects rather than
    // through per-AP RF channel numbers such as 36, 100, and 149.

    // Configure PHY
    YansWifiPhyHelper phyHelper;
    phyHelper.SetChannel(channel);
    phyHelper.Set("ChannelSettings",
                  StringValue("{0, " + std::to_string(bandwidthMhz) +
                              ", BAND_5GHZ, 0}"));

    // Configure MAC (802.11ax)
    WifiHelper wifiHelper;
    wifiHelper.SetStandard(WIFI_STANDARD_80211ax);
    wifiHelper.SetRemoteStationManager("ns3::MinstrelHtWifiManager");

    // Each hard-assigned AP group has a unique SSID. Stations actively probe
    // for that SSID and associate with the AP in their isolated radio domain.
    const std::string ssidName = "llm-ap-" + std::to_string(apIndex);
    const Ssid ssid(ssidName);

    WifiMacHelper macHelper;
    macHelper.SetType("ns3::ApWifiMac",
                      "Ssid", SsidValue(ssid));

    WifiMacHelper staMacHelper;
    staMacHelper.SetType("ns3::StaWifiMac",
                         "Ssid", SsidValue(ssid),
                         "ActiveProbing", BooleanValue(true));

    // Install devices on AP
    NetDeviceContainer apDevices =
        wifiHelper.Install(phyHelper, macHelper, apNode);

    // Install devices on stations
    NetDeviceContainer staDevices =
        wifiHelper.Install(phyHelper, staMacHelper, stationNodes);

    // Log successful Wi-Fi associations. These traces distinguish actual
    // 802.11 association from a later TCP connection callback.
    for (uint32_t i = 0; i < staDevices.GetN(); ++i)
    {
        Ptr<WifiNetDevice> wifiDevice = DynamicCast<WifiNetDevice>(staDevices.Get(i));
        NS_ABORT_MSG_IF(!wifiDevice,
                        "STA device " << i << " in AP group " << apIndex
                                      << " is not a WifiNetDevice");

        Ptr<StaWifiMac> staMac = DynamicCast<StaWifiMac>(wifiDevice->GetMac());
        NS_ABORT_MSG_IF(!staMac,
                        "STA device " << i << " in AP group " << apIndex
                                      << " does not use StaWifiMac");

        staMac->TraceConnectWithoutContext(
            "Assoc",
            MakeBoundCallback(&StaAssociated, apIndex, i));
    }

    // Mobility: fixed positions
    MobilityHelper mobility;

    // AP at center
    mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    mobility.Install(apNode);

    // Set AP position to center
    Ptr<ConstantPositionMobilityModel> apMob =
        apNode.Get(0)->GetObject<ConstantPositionMobilityModel>();
    apMob->SetPosition(Vector(100 * apIndex, 100 * apIndex, 100 * apIndex));

    // Stations in a disc around AP (5m radius)
    // mobility.SetPositionAllocator("ns3::RandomDiscPositionAllocator",
    //                               "Center", VectorValue(Vector(100 * apIndex, 100 * apIndex, 100 * apIndex)),
    //                               "MaxDistance", DoubleValue(5.0));
    mobility.SetPositionAllocator("ns3::UniformDiscPositionAllocator",
                                  "X", DoubleValue(100 * apIndex),
                                  "Y", DoubleValue(100 * apIndex),
                                  "Z", DoubleValue(100 * apIndex),
                                  "rho", DoubleValue(5.0));
    mobility.Install(stationNodes);

    // Internet stack
    InternetStackHelper stack;
    stack.Install(apNode);
    stack.Install(stationNodes);

    // Assign AP first, then stations. The AP receives 10.1.<apIndex>.1.
    Ipv4AddressHelper ipv4;
    std::string subnet = "10.1." + std::to_string(apIndex) + ".0";
    ipv4.SetBase(Ipv4Address(subnet.c_str()), Ipv4Mask("255.255.255.0"));

    NS_LOG_INFO("Number of devices in apDevices: " << apDevices.GetN());
    Ipv4InterfaceContainer apInterfaces = ipv4.Assign(apDevices);
    Ipv4InterfaceContainer staInterfaces = ipv4.Assign(staDevices);

    wifiStatistics.RegisterApGroup(apIndex, apInterfaces.GetAddress(0), staInterfaces);

    wifiStatistics.RegisterWifiDevice(
        apNode.Get(0)->GetId(),
        "AP" + std::to_string(apIndex) + "(" +
            Ipv4ToString(apInterfaces.GetAddress(0)) + ")",
        apDevices.Get(0));

    for (uint32_t i = 0; i < staDevices.GetN(); ++i)
    {
        wifiStatistics.RegisterWifiDevice(
            stationNodes.Get(i)->GetId(),
            "AP" + std::to_string(apIndex) + "/STA" + std::to_string(i) + "(" +
                Ipv4ToString(staInterfaces.GetAddress(i)) + ")",
            staDevices.Get(i));
    }

    NS_LOG_INFO("AP location X:" << 100 * apIndex
                << " Y:" << 100 * apIndex
                << " Z:" << 100 * apIndex
                << ", stations randomly within 5m distance");
    NS_LOG_INFO("AP SSID: " << ssidName);
    NS_LOG_INFO("AP IP: " << apInterfaces.GetAddress(0));
    NS_LOG_INFO("STA N: " << staInterfaces.GetN());
    NS_LOG_INFO("STA IP1: " << staInterfaces.GetAddress(0));
    NS_LOG_INFO("STA IP2: " << staInterfaces.GetAddress(1));
    NS_LOG_INFO("STA IP3: " << staInterfaces.GetAddress(2));

    // ========================================================================
    // Install TrafficSink on AP (receives uplink from stations)
    // ========================================================================
    Ptr<TrafficSink> apSink = CreateObject<TrafficSink>();
    apSink->SetAttribute("Port", UintegerValue(10000));
    apNode.Get(0)->AddApplication(apSink);
    apSink->SetStartTime(Seconds(0));
    trafficCoordinator.AddApplication(apSink);

    // ========================================================================
    // Install TrafficSink on each station (receives downlink from AP)
    // ========================================================================
    for (uint32_t i = 0; i < stationNodes.GetN(); i++)
    {
        Ptr<TrafficSink> sink = CreateObject<TrafficSink>();
        sink->SetAttribute("Port", UintegerValue(9000 + i));
        stationNodes.Get(i)->AddApplication(sink);
        sink->SetStartTime(Seconds(0));
        trafficCoordinator.AddApplication(sink);
    }

    // Starting a generator only starts TCP setup. Payload scheduling is held
    // behind the global barrier until every generator reports ready.
    const Time trafficGeneratorStart = Seconds(1.0);

    // ========================================================================
    // Install StaLlmGenerator on stations (uplink to AP)
    // One StaLlmGenerator per station, handling ALL agents on that station
    // Uses single TCP socket per station shared by all agents
    // ========================================================================

    // Group agents by station index
    std::map<int, std::vector<std::pair<std::string, std::vector<Operation>>>> stationToAgents;
    for (const auto& [agentKey, stationAddr] : agentStationMap)
    {
        auto addr = InetSocketAddress::ConvertFrom(stationAddr);
        Ipv4Address ip = addr.GetIpv4();
        uint32_t ipv4 = ip.Get();
        uint32_t lastOctet = ipv4 & 0xff;
        int stationIndex = static_cast<int>(lastOctet - 2);

        NS_ABORT_MSG_IF(stationIndex < 0 ||
                            stationIndex >= static_cast<int>(stationNodes.GetN()),
                        "Invalid station index " << stationIndex
                                                 << " for agent " << agentKey
                                                 << " in AP group " << apIndex);

        stationToAgents[stationIndex].push_back({agentKey, agentMap.at(agentKey)});
    }

    // Create one StaLlmGenerator per station that has agents
    for (auto& [idx, agents] : stationToAgents)
    {
        Ptr<StaLlmGenerator> gen = CreateObject<StaLlmGenerator>();
        gen->SetAttribute("Remote", AddressValue(apAddress));
        gen->SetReadyCallback(trafficCoordinator.GetReadyCallback());

        // Merge all operations from all agents on this station
        std::map<std::string, std::vector<std::tuple<int, double, double, int>>> agentOpsMap;
        for (const auto& [aKey, ops] : agents)
        {
            for (const auto& op : ops)
            {
                agentOpsMap[aKey].push_back(
                    std::make_tuple(op.downlinkBytes,
                                    op.endMs,
                                    op.startOffsetMs,
                                    op.uplinkBytes));
            }
        }
        gen->SetAgentMap(agentOpsMap);

        const uint32_t staNodeId = stationNodes.Get(idx)->GetId();
        wifiStatistics.ConnectStaGenerator(gen, staNodeId);

        stationNodes.Get(idx)->AddApplication(gen);
        gen->SetStartTime(trafficGeneratorStart);

        trafficCoordinator.AddGenerator(gen);
        trafficCoordinator.AddApplication(gen);

        NS_LOG_INFO("Station " << idx << " placed on node "
                    << idx << " with " << agents.size() << " agents");
    }

    // ========================================================================
    // Install APGenerator on AP (downlink to stations)
    // Uses agentStationMap directly with string keys
    // ========================================================================
    Ptr<APGenerator> apGen = CreateObject<APGenerator>();
    apGen->SetReadyCallback(trafficCoordinator.GetReadyCallback());

    // Pass operations directly - tuple format: (downlinkBytes, endMs, startOffsetMs, uplinkBytes)
    std::map<std::string, std::vector<std::tuple<int, double, double, int>>> rawOpsMap;
    for (const auto& [agentKey, ops] : agentMap)
    {
        for (const auto& op : ops)
        {
            rawOpsMap[agentKey].push_back(
                std::make_tuple(op.downlinkBytes,
                                op.endMs,
                                op.startOffsetMs,
                                op.uplinkBytes));
        }
    }
    // Pass station map directly (already uses string keys from agent-distribution)
    apGen->SetAgentStationMap(agentStationMap);
    apGen->SetAgentMap(rawOpsMap);

    const uint32_t apNodeId = apNode.Get(0)->GetId();
    wifiStatistics.ConnectApGenerator(apGen, apNodeId);

    apNode.Get(0)->AddApplication(apGen);
    apGen->SetStartTime(trafficGeneratorStart);

    trafficCoordinator.AddGenerator(apGen);
    trafficCoordinator.AddApplication(apGen);

    NS_LOG_INFO("AP group " << apIndex << " setup complete: "
                << agentMap.size() << " agents, " << staNum << " stations");
}

// ============================================================================
//
// ============================================================================

int
main(int argc, char* argv[])
{
    RngSeedManager::SetSeed(12345);
    RngSeedManager::SetRun(1);

    // TcpCubic
    // TcpHighSpeed
    // TcpBbr
    // TcpLinuxReno
    Config::SetDefault("ns3::TcpL4Protocol::SocketType", TypeIdValue(TcpHighSpeed::GetTypeId()));

    // Parse command line
    std::string jsonPath;
    std::string statsOutputPath = "mac-node-stats.json";
    std::string experimentTimeArg = "auto";
    bool autoExperimentTime = true;
    double fixedExperimentTimeMs = 0.0;
    int bandwidthMhz = 20;
    int apNum = 3;
    int staNum = 30;

    if (argc < 2)
    {
        std::cerr << "Usage: " << argv[0]
                  << " <traces.json> [bandwidth_mhz] [stats_output.json] [experiment_time]"
                  << "\n  bandwidth_mhz: 20, 40, 80 or 160 (default: 20)"
                  << "\n  stats_output.json: default mac-node-stats.json"
                  << "\n  experiment_time: auto (JSON duration + 2s, default) or fixed seconds > 0"
                  << std::endl;
        return 1;
    }

    jsonPath = argv[1];
    if (argc >= 3)
    {
        bandwidthMhz = std::stoi(argv[2]);
    }
    if (argc >= 4)
    {
        statsOutputPath = argv[3];
    }
    if (argc >= 5)
    {
        experimentTimeArg = argv[4];

        if (experimentTimeArg != "auto")
        {
            try
            {
                std::size_t parsedChars = 0;
                const double fixedExperimentTimeSeconds =
                    std::stod(experimentTimeArg, &parsedChars);

                if (parsedChars != experimentTimeArg.size() ||
                    !std::isfinite(fixedExperimentTimeSeconds) ||
                    fixedExperimentTimeSeconds <= 0.0)
                {
                    throw std::invalid_argument("invalid experiment time");
                }

                autoExperimentTime = false;
                fixedExperimentTimeMs = fixedExperimentTimeSeconds * 1000.0;
            }
            catch (const std::exception&)
            {
                std::cerr << "Invalid experiment_time: " << experimentTimeArg
                          << ". Expected 'auto' or a positive number of seconds."
                          << std::endl;
                return 1;
            }
        }
    }
    if (argc > 5)
    {
        std::cerr << "Too many command-line arguments." << std::endl;
        return 1;
    }

    if (bandwidthMhz != 20 &&
        bandwidthMhz != 40 &&
        bandwidthMhz != 80 &&
        bandwidthMhz != 160)
    {
        std::cerr << "Unsupported bandwidth: " << bandwidthMhz
                  << " MHz. Expected 20, 40, 80 or 160." << std::endl;
        return 1;
    }

    std::cout << "=== ns-3 Sample Scenario: " << apNum << " APs x "
              << staNum << " Stations ===" << std::endl;
    std::cout << "JSON: " << jsonPath << std::endl;
    std::cout << "Bandwidth: " << bandwidthMhz << " MHz" << std::endl;
    std::cout << "MAC stats JSON: " << statsOutputPath << std::endl;
    std::cout << "Standard: 802.11ax (Wi-Fi 6)" << std::endl;
    std::cout << "Transport: TCP" << std::endl;
    std::cout << "Channel model: separate YansWifiChannel per AP group" << std::endl;
    std::cout << "Channel policy: physically isolated AP groups; default 5 GHz channel"
              << std::endl;

    // Enable logging
    LogComponentEnable("SampleScenario", LOG_LEVEL_INFO);
    LogComponentEnable("APGenerator", LOG_LEVEL_WARN);
    LogComponentEnable("StaLlmGenerator", LOG_LEVEL_WARN);
    LogComponentEnable("TrafficSink", LOG_LEVEL_WARN);
    // LogComponentEnable("AgentDistribution", LOG_LEVEL_INFO);
    LogComponentEnable("ContentionAwareAgentDistribution", LOG_LEVEL_INFO);

    // Parse and distribute agents
    ParsedResult parsed = ParseJsonFile(jsonPath);
    const double traceDurationMs = parsed.experimentDurationMs;
    const double maxExperimentDurationMs =
        autoExperimentTime ? traceDurationMs + kAutoExperimentTailMarginMs
                           : fixedExperimentTimeMs;
    TrafficCoordinator trafficCoordinator(traceDurationMs, maxExperimentDurationMs);
    g_trafficCoordinator = &trafficCoordinator;
    WifiStatistics wifiStatistics(trafficCoordinator);
    g_wifiStatistics = &wifiStatistics;

    // Default distribution
    // DistributionResult dist = DistributeAgents(parsed, apNum, staNum, 3);

    // Distribution with contention awareness
    ContentionAwareDistributionConfig distributionConfig;

    distributionConfig.nAp = apNum;
    distributionConfig.nStationsPerAp = staNum;
    // 0 = unlimited application-level agents per physical STA.
    distributionConfig.maxAgentsPerStation = 832;

    // true:
    //   contention важнее количества используемых STA.
    //
    // false:
    //   сначала стараемся задействовать максимально возможное число STA.
    distributionConfig.lowContentionPriority = true;

    // Размер окна для приблизительного определения одновременного UL.
    // Можно свободно менять для экспериментов.
    distributionConfig.slotMs = 10;

    DistributionResult dist =
        DistributeAgentsContentionAware(
            parsed,
            distributionConfig);

    // Set up each AP group
    for (int ap = 0; ap < apNum; ap++)
    {
        SetupApGroup(ap,
                     bandwidthMhz,
                     dist.apStationMaps[ap],
                     dist.apAgentMaps[ap],
                     dist.apAddresses[ap],
                     staNum,
                     trafficCoordinator,
                     wifiStatistics);
    }

    trafficCoordinator.FinalizeRegistration();

    if (autoExperimentTime)
    {
        std::cout << "\nExperiment time mode: auto" << std::endl;
        std::cout << "Experiment duration from JSON: "
                  << g_trafficCoordinator->GetTraceDurationMs() / 1000.0 << " seconds" << std::endl;
        std::cout << "Max experiment time: "
                  << g_trafficCoordinator->GetMaxExperimentDurationMs() / 1000.0
                  << " seconds (JSON duration + "
                  << kAutoExperimentTailMarginMs / 1000.0
                  << " seconds)" << std::endl;
    }
    else
    {
        std::cout << "\nExperiment time mode: fixed" << std::endl;
        std::cout << "Max experiment time: "
                  << g_trafficCoordinator->GetMaxExperimentDurationMs() / 1000.0
                  << " seconds" << std::endl;
        std::cout << "Experiment duration from JSON: "
                  << g_trafficCoordinator->GetTraceDurationMs() / 1000.0 << " seconds" << std::endl;

        if (g_trafficCoordinator->GetMaxExperimentDurationMs() < g_trafficCoordinator->GetTraceDurationMs())
        {
            NS_LOG_WARN("Fixed experiment time "
                        << g_trafficCoordinator->GetMaxExperimentDurationMs() / 1000.0
                        << "s is shorter than JSON duration "
                        << g_trafficCoordinator->GetTraceDurationMs() / 1000.0
                        << "s; trace tail will be truncated");
        }
    }
    std::cout << "Waiting for " << trafficCoordinator.GetExpectedGeneratorCount()
              << " traffic generators to complete TCP setup..." << std::endl;

    Config::Connect ("/NodeList/*/DeviceList/*/$ns3::WifiNetDevice/Mac/MacTx",
                 MakeCallback (&DeviceTxTrace));

    Config::Connect ("/NodeList/*/DeviceList/*/$ns3::WifiNetDevice/Mac/MacRx",
                 MakeCallback (&DeviceRxTrace));

    Config::SetDefault(
    "ns3::TcpL4Protocol::SocketType",
    TypeIdValue(TcpLinuxReno::GetTypeId()));
    Config::SetDefault("ns3::TcpSocket::SegmentSize", UintegerValue(1460));
    Config::SetDefault("ns3::TcpSocket::SndBufSize", UintegerValue(32 * 1024 * 1024));
    Config::SetDefault("ns3::TcpSocket::RcvBufSize", UintegerValue(32 * 1024 * 1024));

    const auto wallClockStart = std::chrono::steady_clock::now();

    Simulator::Run();

    const auto wallClockEnd = std::chrono::steady_clock::now();
    const double wallClockSeconds =
        std::chrono::duration<double>(wallClockEnd - wallClockStart).count();

    NS_ABORT_MSG_IF(g_trafficCoordinator->GetExperimentStartUs() < 0,
                    "Simulation ended before the global traffic barrier opened");

    wifiStatistics.WriteJson(statsOutputPath);
    PrintTransmissionTimePerSender();
    wifiStatistics.PrintCrossLayerReport();

    Simulator::Destroy();

    std::cout << "\n=== Simulation Complete ===" << std::endl;
    std::cout << "Total events: " << Simulator::GetEventCount() << std::endl;
    NS_LOG_INFO("[Realtime] Simulator::Run wall-clock time: "
                << std::fixed << std::setprecision(3)
                << wallClockSeconds << " seconds");
    std::cout << "Realtime simulation runtime: "
              << std::fixed << std::setprecision(3)
              << wallClockSeconds << " seconds" << std::endl;

    return 0;
}
