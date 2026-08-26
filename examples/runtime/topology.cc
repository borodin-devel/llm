#include "topology.h"

#include "../statistics/experiment-statistics.h"
#include "log.h"
#include "traffic-coordinator.h"

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
                  const TopologyConfig& topologyConfig,
                  const NodeContainer& accessPointNode,
                  const NodeContainer& stationNodes)
{
    MobilityHelper mobility;
    mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    mobility.Install(accessPointNode);

    Ptr<ConstantPositionMobilityModel> accessPointMobility =
        accessPointNode.Get(0)->GetObject<ConstantPositionMobilityModel>();
    const double coordinate = topologyConfig.bssSpacingM * bssIndex;
    accessPointMobility->SetPosition(Vector(coordinate, coordinate, coordinate));

    mobility.SetPositionAllocator("ns3::UniformDiscPositionAllocator",
                                  "X",
                                  DoubleValue(coordinate),
                                  "Y",
                                  DoubleValue(coordinate),
                                  "Z",
                                  DoubleValue(coordinate),
                                  "rho",
                                  DoubleValue(topologyConfig.stationRadiusM));
    mobility.Install(stationNodes);
}

BssTopology
CreateBssTopology(int bssIndex,
                  const TopologyConfig& topologyConfig,
                  const WifiConfig& wifiConfig,
                  Ptr<YansWifiChannel> sharedChannel)
{
    BssTopology topology;
    topology.accessPointNode.Create(1);
    topology.stationNodes.Create(topologyConfig.stationsPerBss);

    Ptr<YansWifiChannel> channel =
        SelectBssChannel(topologyConfig.isolateBssChannels, sharedChannel);

    YansWifiPhyHelper phyHelper;
    phyHelper.SetChannel(channel);
    phyHelper.Set("ChannelSettings", StringValue(BuildChannelSettings(wifiConfig)));

    WifiHelper wifiHelper;
    wifiHelper.SetStandard(WIFI_STANDARD_80211ax);
    wifiHelper.SetRemoteStationManager(wifiConfig.rateManager);

    topology.ssidName = topologyConfig.ssidPrefix + std::to_string(bssIndex);
    const Ssid ssid(topology.ssidName);

    WifiMacHelper accessPointMacHelper;
    accessPointMacHelper.SetType("ns3::ApWifiMac", "Ssid", SsidValue(ssid));

    WifiMacHelper stationMacHelper;
    stationMacHelper.SetType("ns3::StaWifiMac",
                             "Ssid",
                             SsidValue(ssid),
                             "ActiveProbing",
                             BooleanValue(wifiConfig.activeProbing));

    topology.accessPointDevices =
        wifiHelper.Install(phyHelper, accessPointMacHelper, topology.accessPointNode);
    topology.stationDevices =
        wifiHelper.Install(phyHelper, stationMacHelper, topology.stationNodes);

    ConnectAssociationTraces(bssIndex, topology.stationDevices);
    ConfigureMobility(bssIndex, topologyConfig, topology.accessPointNode, topology.stationNodes);

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
RegisterTopology(int bssIndex,
                 const TopologyConfig& topologyConfig,
                 const BssTopology& topology,
                 ExperimentStatistics& statistics)
{
    const std::string accessPointLabel =
        "AP" + std::to_string(bssIndex) + "(" +
        Ipv4ToString(topology.accessPointInterfaces.GetAddress(0)) + ")";
    statistics.RegisterAccessPointIdentity(bssIndex,
                                           topology.accessPointNode.Get(0)->GetId(),
                                           accessPointLabel,
                                           topology.accessPointInterfaces.GetAddress(0));
    statistics.RegisterWifiDevice(topology.accessPointNode.Get(0)->GetId(),
                                  topology.accessPointDevices.Get(0));

    for (uint32_t stationIndex = 0; stationIndex < topology.stationDevices.GetN(); ++stationIndex)
    {
        const std::string stationLabel =
            "AP" + std::to_string(bssIndex) + "/STA" + std::to_string(stationIndex) + "(" +
            Ipv4ToString(topology.stationInterfaces.GetAddress(stationIndex)) + ")";
        statistics.RegisterStationIdentity(bssIndex,
                                           stationIndex,
                                           topology.stationNodes.Get(stationIndex)->GetId(),
                                           stationLabel,
                                           topology.stationInterfaces.GetAddress(stationIndex));
        statistics.RegisterWifiDevice(topology.stationNodes.Get(stationIndex)->GetId(),
                                      topology.stationDevices.Get(stationIndex));
    }

    const double coordinate = topologyConfig.bssSpacingM * bssIndex;
    NS_LOG_INFO("AP location X:" << coordinate << " Y:" << coordinate << " Z:" << coordinate
                                 << ", stations randomly within " << topologyConfig.stationRadiusM
                                 << "m distance");
    NS_LOG_INFO("AP SSID: " << topology.ssidName);
    NS_LOG_INFO("AP IP: " << topology.accessPointInterfaces.GetAddress(0));
    NS_LOG_INFO("STA N: " << topology.stationInterfaces.GetN());
}

void
InstallTrafficSinks(const TopologyConfig& topologyConfig,
                    const BssTopology& topology,
                    TrafficCoordinator& coordinator,
                    ExperimentStatistics& statistics)
{
    Ptr<TrafficSink> accessPointSink = CreateObject<TrafficSink>();
    accessPointSink->SetAttribute("Port", UintegerValue(topologyConfig.apSinkPort));
    statistics.ConnectTrafficSink(accessPointSink,
                                  topology.accessPointNode.Get(0)->GetId(),
                                  ExperimentDirection::UPLINK);
    topology.accessPointNode.Get(0)->AddApplication(accessPointSink);
    accessPointSink->SetStartTime(Seconds(0));
    coordinator.AddApplication(accessPointSink);

    for (uint32_t stationIndex = 0; stationIndex < topology.stationNodes.GetN(); ++stationIndex)
    {
        Ptr<TrafficSink> stationSink = CreateObject<TrafficSink>();
        stationSink->SetAttribute(
            "Port",
            UintegerValue(static_cast<uint32_t>(topologyConfig.stationSinkBasePort) +
                          stationIndex));
        statistics.ConnectTrafficSink(stationSink,
                                      topology.stationNodes.Get(stationIndex)->GetId(),
                                      ExperimentDirection::DOWNLINK);
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

Address
SetSocketPort(Address address, uint16_t port)
{
    const auto socketAddress = InetSocketAddress::ConvertFrom(address);
    return InetSocketAddress(socketAddress.GetIpv4(), port);
}

std::map<std::string, Address>
ConfigureStationSinkAddresses(const TopologyConfig& topologyConfig,
                              const std::map<std::string, Address>& stationAddressByAgent)
{
    std::map<std::string, Address> configuredAddresses;
    for (const auto& [agentKey, stationAddress] : stationAddressByAgent)
    {
        const auto socketAddress = InetSocketAddress::ConvertFrom(stationAddress);
        const uint32_t stationIndex = (socketAddress.GetIpv4().Get() & 0xff) - 2;
        configuredAddresses.emplace(
            agentKey,
            InetSocketAddress(socketAddress.GetIpv4(),
                              topologyConfig.stationSinkBasePort + stationIndex));
    }
    return configuredAddresses;
}

void
InstallStationGenerators(Address accessPointAddress,
                         const AgentKeysByStation& agentKeysByStation,
                         const LegacyAgentOperations& operationsByAgent,
                         const NodeContainer& stationNodes,
                         Time generatorStart,
                         TrafficCoordinator& coordinator,
                         ExperimentStatistics& statistics)
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
                            ExperimentStatistics& statistics)
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

Ptr<YansWifiChannel>
CreateDefaultYansChannel()
{
    return YansWifiChannelHelper::Default().Create();
}

Ptr<YansWifiChannel>
SelectBssChannel(bool isolateBssChannels, Ptr<YansWifiChannel> sharedChannel)
{
    return isolateBssChannels ? CreateDefaultYansChannel() : sharedChannel;
}

std::string
BuildChannelSettings(const WifiConfig& wifiConfig)
{
    std::string bandName;
    switch (ToWifiPhyBand(wifiConfig.band))
    {
    case WIFI_PHY_BAND_2_4GHZ:
        bandName = "BAND_2_4GHZ";
        break;
    case WIFI_PHY_BAND_5GHZ:
        bandName = "BAND_5GHZ";
        break;
    case WIFI_PHY_BAND_6GHZ:
        bandName = "BAND_6GHZ";
        break;
    default:
        NS_ABORT_MSG("Unsupported Wi-Fi band");
    }

    return "{" + std::to_string(wifiConfig.channelNumber) + ", " +
           std::to_string(wifiConfig.bandwidthMhz) + ", " + bandName + ", " +
           std::to_string(wifiConfig.primary20Index) + "}";
}

void
SetupApGroup(int bssIndex,
             const TopologyConfig& topologyConfig,
             const WifiConfig& wifiConfig,
             Ptr<YansWifiChannel> sharedChannel,
             const std::map<std::string, Address>& stationAddressByAgent,
             const AgentOperations& operationsByAgent,
             Address accessPointAddress,
             TrafficCoordinator& coordinator,
             ExperimentStatistics& statistics)
{
    NS_LOG_INFO("=== Setting up AP group " << bssIndex << ", BW " << wifiConfig.bandwidthMhz
                                           << " MHz ===");

    const BssTopology topology =
        CreateBssTopology(bssIndex, topologyConfig, wifiConfig, sharedChannel);
    RegisterTopology(bssIndex, topologyConfig, topology, statistics);
    InstallTrafficSinks(topologyConfig, topology, coordinator, statistics);

    // Generator start opens TCP setup; payload remains behind the common barrier.
    const Time generatorStart = Seconds(topologyConfig.generatorStartSeconds);
    const LegacyAgentOperations legacyOperations = ConvertOperations(operationsByAgent);
    const AgentKeysByStation agentKeysByStation =
        GroupAgentKeysByStation(bssIndex, topologyConfig.stationsPerBss, stationAddressByAgent);
    const Address configuredAccessPointAddress =
        SetSocketPort(accessPointAddress, topologyConfig.apSinkPort);
    const auto configuredStationAddresses =
        ConfigureStationSinkAddresses(topologyConfig, stationAddressByAgent);

    InstallStationGenerators(configuredAccessPointAddress,
                             agentKeysByStation,
                             legacyOperations,
                             topology.stationNodes,
                             generatorStart,
                             coordinator,
                             statistics);
    InstallAccessPointGenerator(configuredStationAddresses,
                                legacyOperations,
                                topology,
                                generatorStart,
                                coordinator,
                                statistics);

    NS_LOG_INFO("AP group " << bssIndex << " setup complete: " << operationsByAgent.size()
                            << " agents, " << topologyConfig.stationsPerBss << " stations");
}

} // namespace ns3
