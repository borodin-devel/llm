#include "scenario-topology.h"

#include "scenario-log.h"
#include "traffic-coordinator.h"
#include "wifi-statistics.h"

#include "ns3/ap-generator.h"
#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/internet-module.h"
#include "ns3/mobility-module.h"
#include "ns3/network-module.h"
#include "ns3/sta-llm-generator.h"
#include "ns3/traffic-sink.h"
#include "ns3/wifi-module.h"
#include "ns3/yans-wifi-helper.h"

#include <cstdint>
#include <map>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace ns3
{

namespace
{

LogComponent& g_log = llm_example::GetScenarioLog();

using AgentOperations = std::map<std::string, std::vector<Operation>>;
using LegacyOperation = std::tuple<int, double, double, int>;
using LegacyAgentOperations = std::map<std::string, std::vector<LegacyOperation>>;
using AgentKeysByStation = std::map<int, std::vector<std::string>>;

struct BssTopology
{
    NodeContainer accessPointNode;
    NodeContainer stationNodes;
    NetDeviceContainer accessPointDevices;
    NetDeviceContainer stationDevices;
    Ipv4InterfaceContainer accessPointInterfaces;
    Ipv4InterfaceContainer stationInterfaces;
    std::string ssidName;
};

std::string
Ipv4ToString(Ipv4Address address)
{
    std::ostringstream stream;
    address.Print(stream);
    return stream.str();
}

void
StaAssociated(int bssIndex, uint32_t stationIndex, Mac48Address bssid)
{
    NS_LOG_INFO("[Wi-Fi association] AP group " << bssIndex << " STA " << stationIndex
                                                << " associated with BSSID " << bssid);
}

void
ConnectAssociationTraces(int bssIndex, const NetDeviceContainer& stationDevices)
{
    for (uint32_t stationIndex = 0; stationIndex < stationDevices.GetN(); ++stationIndex)
    {
        Ptr<WifiNetDevice> wifiDevice =
            DynamicCast<WifiNetDevice>(stationDevices.Get(stationIndex));
        NS_ABORT_MSG_IF(!wifiDevice,
                        "STA device " << stationIndex << " in AP group " << bssIndex
                                      << " is not a WifiNetDevice");

        Ptr<StaWifiMac> stationMac = DynamicCast<StaWifiMac>(wifiDevice->GetMac());
        NS_ABORT_MSG_IF(!stationMac,
                        "STA device " << stationIndex << " in AP group " << bssIndex
                                      << " does not use StaWifiMac");

        stationMac->TraceConnectWithoutContext(
            "Assoc",
            MakeBoundCallback(&StaAssociated, bssIndex, stationIndex));
    }
}

void
ConfigureMobility(int bssIndex,
                  const NodeContainer& accessPointNode,
                  const NodeContainer& stationNodes)
{
    MobilityHelper mobility;
    mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    mobility.Install(accessPointNode);

    Ptr<ConstantPositionMobilityModel> accessPointMobility =
        accessPointNode.Get(0)->GetObject<ConstantPositionMobilityModel>();
    accessPointMobility->SetPosition(Vector(100 * bssIndex, 100 * bssIndex, 100 * bssIndex));

    mobility.SetPositionAllocator("ns3::UniformDiscPositionAllocator",
                                  "X",
                                  DoubleValue(100 * bssIndex),
                                  "Y",
                                  DoubleValue(100 * bssIndex),
                                  "Z",
                                  DoubleValue(100 * bssIndex),
                                  "rho",
                                  DoubleValue(5.0));
    mobility.Install(stationNodes);
}

BssTopology
CreateBssTopology(int bssIndex, int bandwidthMhz, uint32_t stationCount)
{
    BssTopology topology;
    topology.accessPointNode.Create(1);
    topology.stationNodes.Create(stationCount);

    // Independent channel objects make AP groups physically isolated.
    YansWifiChannelHelper channelHelper = YansWifiChannelHelper::Default();
    Ptr<YansWifiChannel> channel = channelHelper.Create();

    YansWifiPhyHelper phyHelper;
    phyHelper.SetChannel(channel);
    phyHelper.Set("ChannelSettings",
                  StringValue("{0, " + std::to_string(bandwidthMhz) + ", BAND_5GHZ, 0}"));

    WifiHelper wifiHelper;
    wifiHelper.SetStandard(WIFI_STANDARD_80211ax);
    wifiHelper.SetRemoteStationManager("ns3::MinstrelHtWifiManager");

    topology.ssidName = "llm-ap-" + std::to_string(bssIndex);
    const Ssid ssid(topology.ssidName);

    WifiMacHelper accessPointMacHelper;
    accessPointMacHelper.SetType("ns3::ApWifiMac", "Ssid", SsidValue(ssid));

    WifiMacHelper stationMacHelper;
    stationMacHelper.SetType("ns3::StaWifiMac",
                             "Ssid",
                             SsidValue(ssid),
                             "ActiveProbing",
                             BooleanValue(true));

    topology.accessPointDevices =
        wifiHelper.Install(phyHelper, accessPointMacHelper, topology.accessPointNode);
    topology.stationDevices =
        wifiHelper.Install(phyHelper, stationMacHelper, topology.stationNodes);

    ConnectAssociationTraces(bssIndex, topology.stationDevices);
    ConfigureMobility(bssIndex, topology.accessPointNode, topology.stationNodes);

    InternetStackHelper stack;
    stack.Install(topology.accessPointNode);
    stack.Install(topology.stationNodes);

    Ipv4AddressHelper ipv4;
    const std::string subnet = "10.1." + std::to_string(bssIndex) + ".0";
    ipv4.SetBase(Ipv4Address(subnet.c_str()), Ipv4Mask("255.255.255.0"));

    NS_LOG_INFO("Number of devices in apDevices: " << topology.accessPointDevices.GetN());
    topology.accessPointInterfaces = ipv4.Assign(topology.accessPointDevices);
    topology.stationInterfaces = ipv4.Assign(topology.stationDevices);
    return topology;
}

void
RegisterTopology(int bssIndex, const BssTopology& topology, WifiStatistics& statistics)
{
    statistics.RegisterApGroup(bssIndex,
                               topology.accessPointInterfaces.GetAddress(0),
                               topology.stationInterfaces);

    statistics.RegisterWifiDevice(topology.accessPointNode.Get(0)->GetId(),
                                  "AP" + std::to_string(bssIndex) + "(" +
                                      Ipv4ToString(topology.accessPointInterfaces.GetAddress(0)) +
                                      ")",
                                  topology.accessPointDevices.Get(0));

    for (uint32_t stationIndex = 0; stationIndex < topology.stationDevices.GetN(); ++stationIndex)
    {
        statistics.RegisterWifiDevice(
            topology.stationNodes.Get(stationIndex)->GetId(),
            "AP" + std::to_string(bssIndex) + "/STA" + std::to_string(stationIndex) + "(" +
                Ipv4ToString(topology.stationInterfaces.GetAddress(stationIndex)) + ")",
            topology.stationDevices.Get(stationIndex));
    }

    NS_LOG_INFO("AP location X:" << 100 * bssIndex << " Y:" << 100 * bssIndex << " Z:"
                                 << 100 * bssIndex << ", stations randomly within 5m distance");
    NS_LOG_INFO("AP SSID: " << topology.ssidName);
    NS_LOG_INFO("AP IP: " << topology.accessPointInterfaces.GetAddress(0));
    NS_LOG_INFO("STA N: " << topology.stationInterfaces.GetN());
    NS_LOG_INFO("STA IP1: " << topology.stationInterfaces.GetAddress(0));
    NS_LOG_INFO("STA IP2: " << topology.stationInterfaces.GetAddress(1));
    NS_LOG_INFO("STA IP3: " << topology.stationInterfaces.GetAddress(2));
}

void
InstallTrafficSinks(const BssTopology& topology, TrafficCoordinator& coordinator)
{
    Ptr<TrafficSink> accessPointSink = CreateObject<TrafficSink>();
    accessPointSink->SetAttribute("Port", UintegerValue(10000));
    topology.accessPointNode.Get(0)->AddApplication(accessPointSink);
    accessPointSink->SetStartTime(Seconds(0));
    coordinator.AddApplication(accessPointSink);

    for (uint32_t stationIndex = 0; stationIndex < topology.stationNodes.GetN(); ++stationIndex)
    {
        Ptr<TrafficSink> stationSink = CreateObject<TrafficSink>();
        stationSink->SetAttribute("Port", UintegerValue(9000 + stationIndex));
        topology.stationNodes.Get(stationIndex)->AddApplication(stationSink);
        stationSink->SetStartTime(Seconds(0));
        coordinator.AddApplication(stationSink);
    }
}

LegacyAgentOperations
ConvertOperations(const AgentOperations& operationsByAgent)
{
    LegacyAgentOperations converted;
    for (const auto& [agentKey, operations] : operationsByAgent)
    {
        auto& convertedOperations = converted[agentKey];
        for (const auto& operation : operations)
        {
            convertedOperations.emplace_back(operation.downlinkBytes,
                                             operation.endMs,
                                             operation.startOffsetMs,
                                             operation.uplinkBytes);
        }
    }
    return converted;
}

AgentKeysByStation
GroupAgentKeysByStation(int bssIndex,
                        uint32_t stationCount,
                        const std::map<std::string, Address>& stationAddressByAgent)
{
    AgentKeysByStation agentKeysByStation;
    for (const auto& [agentKey, stationAddress] : stationAddressByAgent)
    {
        const auto socketAddress = InetSocketAddress::ConvertFrom(stationAddress);
        const uint32_t lastOctet = socketAddress.GetIpv4().Get() & 0xff;
        const int stationIndex = static_cast<int>(lastOctet - 2);

        NS_ABORT_MSG_IF(stationIndex < 0 || stationIndex >= static_cast<int>(stationCount),
                        "Invalid station index " << stationIndex << " for agent " << agentKey
                                                 << " in AP group " << bssIndex);

        agentKeysByStation[stationIndex].push_back(agentKey);
    }
    return agentKeysByStation;
}

void
InstallStationGenerators(Address accessPointAddress,
                         const AgentKeysByStation& agentKeysByStation,
                         const LegacyAgentOperations& operationsByAgent,
                         const NodeContainer& stationNodes,
                         Time generatorStart,
                         TrafficCoordinator& coordinator,
                         WifiStatistics& statistics)
{
    for (const auto& [stationIndex, agentKeys] : agentKeysByStation)
    {
        Ptr<StaLlmGenerator> generator = CreateObject<StaLlmGenerator>();
        generator->SetAttribute("Remote", AddressValue(accessPointAddress));
        generator->SetReadyCallback(coordinator.GetReadyCallback());

        LegacyAgentOperations stationOperations;
        for (const auto& agentKey : agentKeys)
        {
            stationOperations.emplace(agentKey, operationsByAgent.at(agentKey));
        }
        generator->SetAgentMap(std::move(stationOperations));

        const uint32_t stationNodeId = stationNodes.Get(stationIndex)->GetId();
        statistics.ConnectStaGenerator(generator, stationNodeId);

        stationNodes.Get(stationIndex)->AddApplication(generator);
        generator->SetStartTime(generatorStart);
        coordinator.AddGenerator(generator);
        coordinator.AddApplication(generator);

        NS_LOG_INFO("Station " << stationIndex << " placed on node " << stationIndex << " with "
                               << agentKeys.size() << " agents");
    }
}

void
InstallAccessPointGenerator(const std::map<std::string, Address>& stationAddressByAgent,
                            const LegacyAgentOperations& operationsByAgent,
                            const BssTopology& topology,
                            Time generatorStart,
                            TrafficCoordinator& coordinator,
                            WifiStatistics& statistics)
{
    Ptr<APGenerator> generator = CreateObject<APGenerator>();
    generator->SetReadyCallback(coordinator.GetReadyCallback());
    generator->SetAgentStationMap(stationAddressByAgent);
    generator->SetAgentMap(operationsByAgent);

    const uint32_t accessPointNodeId = topology.accessPointNode.Get(0)->GetId();
    statistics.ConnectApGenerator(generator, accessPointNodeId);

    topology.accessPointNode.Get(0)->AddApplication(generator);
    generator->SetStartTime(generatorStart);
    coordinator.AddGenerator(generator);
    coordinator.AddApplication(generator);
}

} // namespace

void
SetupApGroup(int bssIndex,
             int bandwidthMhz,
             const std::map<std::string, Address>& stationAddressByAgent,
             const AgentOperations& operationsByAgent,
             Address accessPointAddress,
             uint32_t stationCount,
             TrafficCoordinator& coordinator,
             WifiStatistics& statistics)
{
    NS_LOG_INFO("=== Setting up AP group " << bssIndex << ", BW " << bandwidthMhz << " MHz ===");

    const BssTopology topology = CreateBssTopology(bssIndex, bandwidthMhz, stationCount);
    RegisterTopology(bssIndex, topology, statistics);
    InstallTrafficSinks(topology, coordinator);

    // Generator start opens TCP setup; payload remains behind the common barrier.
    const Time generatorStart = Seconds(1.0);
    const LegacyAgentOperations legacyOperations = ConvertOperations(operationsByAgent);
    const AgentKeysByStation agentKeysByStation =
        GroupAgentKeysByStation(bssIndex, stationCount, stationAddressByAgent);

    InstallStationGenerators(accessPointAddress,
                             agentKeysByStation,
                             legacyOperations,
                             topology.stationNodes,
                             generatorStart,
                             coordinator,
                             statistics);
    InstallAccessPointGenerator(stationAddressByAgent,
                                legacyOperations,
                                topology,
                                generatorStart,
                                coordinator,
                                statistics);

    NS_LOG_INFO("AP group " << bssIndex << " setup complete: " << operationsByAgent.size()
                            << " agents, " << stationCount << " stations");
}

} // namespace ns3
