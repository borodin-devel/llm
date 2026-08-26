#include "topology.h"

#include "access-tracking-sta-wifi-mac.h"
#include "bss-link-filter.h"
#include "log.h"
#include "rssi-placement.h"

#include "ns3/ap-wifi-mac.h"
#include "ns3/boolean.h"
#include "ns3/constant-position-mobility-model.h"
#include "ns3/data-rate.h"
#include "ns3/double.h"
#include "ns3/he-configuration.h"
#include "ns3/internet-stack-helper.h"
#include "ns3/ipv4-address-helper.h"
#include "ns3/ipv4-static-routing-helper.h"
#include "ns3/ipv4.h"
#include "ns3/point-to-point-helper.h"
#include "ns3/propagation-delay-model.h"
#include "ns3/propagation-loss-model.h"
#include "ns3/ssid.h"
#include "ns3/string.h"
#include "ns3/uinteger.h"
#include "ns3/wifi-helper.h"
#include "ns3/wifi-mac-helper.h"
#include "ns3/wifi-net-device.h"
#include "ns3/wifi-phy.h"
#include "ns3/yans-wifi-channel.h"
#include "ns3/yans-wifi-helper.h"

// The private header above shares the core header's basename.
// clang-format off
#include "ns3/log.h"
// clang-format on

#include <array>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <string>

namespace ns3
{

namespace
{

LogComponent& g_log = saturated_tcp_example::GetScenarioLog();

constexpr double ACCESS_POINT_TARGET_RSSI_DBM = -50.0;
constexpr double RSSI_VALIDATION_TOLERANCE_DB = 0.5;
constexpr double PI = 3.14159265358979323846;

/** Native channel state retained while radios are registered. */
struct SaturatedChannelState
{
    Ptr<YansWifiChannel> channel;                        ///< Installed Yans channel.
    Ptr<PropagationLossModel> nativeLoss;                ///< Native LogDistance model.
    Ptr<BssLinkFilterPropagationLossModel> filteredLoss; ///< Optional shared BSS filter.
};

/**
 * Convert an IPv4 address to its canonical dotted spelling.
 *
 * @param address Address to format.
 * @return Canonical address spelling.
 */
std::string
Ipv4ToString(Ipv4Address address)
{
    std::ostringstream output;
    address.Print(output);
    return output.str();
}

/**
 * Create one native deterministic Yans channel.
 *
 * @return Channel and its owned native loss model.
 */
SaturatedChannelState
CreateNativeChannel()
{
    SaturatedChannelState state;
    state.nativeLoss = CreateObject<LogDistancePropagationLossModel>();
    state.channel = CreateObject<YansWifiChannel>();
    state.channel->SetPropagationDelayModel(CreateObject<ConstantSpeedPropagationDelayModel>());
    state.channel->SetPropagationLossModel(state.nativeLoss);
    return state;
}

/**
 * Create the one shared AP-only co-channel propagation chain.
 *
 * @return Shared channel, native loss, and outer filtering wrapper.
 */
SaturatedChannelState
CreateFilteredChannel()
{
    SaturatedChannelState state;
    state.nativeLoss = CreateObject<LogDistancePropagationLossModel>();
    state.filteredLoss = CreateObject<BssLinkFilterPropagationLossModel>();
    state.filteredLoss->SetNativeLossModel(state.nativeLoss);
    state.channel = CreateObject<YansWifiChannel>();
    state.channel->SetPropagationDelayModel(CreateObject<ConstantSpeedPropagationDelayModel>());
    state.channel->SetPropagationLossModel(state.filteredLoss);
    return state;
}

/**
 * Install one fixed-position mobility model.
 *
 * @param node Node that owns the radio.
 * @param position Fixed Cartesian position.
 * @return Installed mobility model.
 */
Ptr<ConstantPositionMobilityModel>
InstallPosition(Ptr<Node> node, const Vector& position)
{
    auto mobility = CreateObject<ConstantPositionMobilityModel>();
    mobility->SetPosition(position);
    node->AggregateObject(mobility);
    return mobility;
}

/**
 * Build the exact channel-settings attribute spelling.
 *
 * @param config Wi-Fi configuration.
 * @return ns-3 ChannelSettings tuple.
 */
std::string
BuildChannelSettings(const SaturatedWifiConfig& config)
{
    return "{" + std::to_string(config.channelNumber) + ", " + std::to_string(config.bandwidthMhz) +
           ", BAND_5GHZ, " + std::to_string(config.primary20Index) + "}";
}

/**
 * Configure the exact benchmark Yans PHY attributes.
 *
 * @param helper PHY helper to configure.
 * @param channel BSS channel.
 * @param config Wi-Fi configuration.
 */
void
ConfigurePhy(YansWifiPhyHelper& helper,
             Ptr<YansWifiChannel> channel,
             const SaturatedWifiConfig& config)
{
    helper.SetChannel(channel);
    helper.Set("ChannelSettings", StringValue(BuildChannelSettings(config)));
    helper.Set("TxPowerStart", DoubleValue(config.txPowerDbm));
    helper.Set("TxPowerEnd", DoubleValue(config.txPowerDbm));
    helper.Set("TxPowerLevels", UintegerValue(1));
    helper.Set("Antennas", UintegerValue(config.antennas));
    helper.Set("MaxSupportedTxSpatialStreams", UintegerValue(config.maxTxSpatialStreams));
    helper.Set("MaxSupportedRxSpatialStreams", UintegerValue(config.maxRxSpatialStreams));
}

/**
 * Validate one native allowed link against its placement target.
 *
 * @param nativeLoss Concrete native loss model.
 * @param txPowerDbm Transmit power in dBm.
 * @param targetRssiDbm Required receive power in dBm.
 * @param sender Sender mobility.
 * @param receiver Receiver mobility.
 * @param bssId BSS owning the desired link or sender AP.
 * @param senderNode Sender node.
 * @param receiverNode Receiver node.
 */
void
ValidateNativeRssi(Ptr<PropagationLossModel> nativeLoss,
                   double txPowerDbm,
                   double targetRssiDbm,
                   Ptr<MobilityModel> sender,
                   Ptr<MobilityModel> receiver,
                   uint32_t bssId,
                   Ptr<Node> senderNode,
                   Ptr<Node> receiverNode)
{
    const double actualRssiDbm = nativeLoss->CalcRxPower(txPowerDbm, sender, receiver);
    const double distanceM = sender->GetDistanceFrom(receiver);
    if (!std::isfinite(actualRssiDbm) ||
        std::abs(actualRssiDbm - targetRssiDbm) > RSSI_VALIDATION_TOLERANCE_DB)
    {
        std::ostringstream message;
        message << "saturated native RSSI validation failed for BSS " << bssId << ", node "
                << senderNode->GetId() << " -> node " << receiverNode->GetId() << ": target "
                << targetRssiDbm << " dBm, actual " << actualRssiDbm << " dBm, distance "
                << distanceM << " m";
        throw std::runtime_error(message.str());
    }
}

/**
 * Register every shared-channel radio with the propagation filter.
 *
 * @param topology Built node/device topology.
 * @param filter Shared propagation wrapper.
 */
void
RegisterFilteredRadios(const SaturatedTcpTopology& topology,
                       Ptr<BssLinkFilterPropagationLossModel> filter)
{
    for (const auto& bss : topology.bss)
    {
        filter->RegisterRadio(bss.accessPointNode->GetObject<MobilityModel>(),
                              bss.bssId,
                              SaturatedRadioRole::ACCESS_POINT);
        for (uint32_t stationIndex = 0; stationIndex < bss.stationNodes.GetN(); ++stationIndex)
        {
            filter->RegisterRadio(bss.stationNodes.Get(stationIndex)->GetObject<MobilityModel>(),
                                  bss.bssId,
                                  SaturatedRadioRole::STATION);
        }
    }
}

/**
 * Validate every desired AP/STA link and every native AP/AP triangle edge.
 *
 * @param topology Built topology.
 * @param channels Per-BSS channel state; shared mode repeats one state.
 * @param config Validated configuration.
 */
void
ValidatePlacement(const SaturatedTcpTopology& topology,
                  const std::array<SaturatedChannelState, 3>& channels,
                  const SaturatedTcpConfig& config)
{
    const double stationTarget = GetSaturatedTcpStationTargetRssiDbm(config.benchmark.rssiRange);
    for (uint32_t bssIndex = 0; bssIndex < topology.bss.size(); ++bssIndex)
    {
        const auto& bss = topology.bss.at(bssIndex);
        const auto apMobility = bss.accessPointNode->GetObject<MobilityModel>();
        for (uint32_t stationIndex = 0; stationIndex < bss.stationNodes.GetN(); ++stationIndex)
        {
            const auto stationNode = bss.stationNodes.Get(stationIndex);
            const auto stationMobility = stationNode->GetObject<MobilityModel>();
            ValidateNativeRssi(channels.at(bssIndex).nativeLoss,
                               config.wifi.txPowerDbm,
                               stationTarget,
                               apMobility,
                               stationMobility,
                               bss.bssId,
                               bss.accessPointNode,
                               stationNode);
            ValidateNativeRssi(channels.at(bssIndex).nativeLoss,
                               config.wifi.txPowerDbm,
                               stationTarget,
                               stationMobility,
                               apMobility,
                               bss.bssId,
                               stationNode,
                               bss.accessPointNode);
        }
    }

    for (uint32_t senderBss = 0; senderBss < topology.bss.size(); ++senderBss)
    {
        for (uint32_t receiverBss = senderBss + 1; receiverBss < topology.bss.size(); ++receiverBss)
        {
            const auto& sender = topology.bss.at(senderBss);
            const auto& receiver = topology.bss.at(receiverBss);
            ValidateNativeRssi(channels.at(senderBss).nativeLoss,
                               config.wifi.txPowerDbm,
                               ACCESS_POINT_TARGET_RSSI_DBM,
                               sender.accessPointNode->GetObject<MobilityModel>(),
                               receiver.accessPointNode->GetObject<MobilityModel>(),
                               sender.bssId,
                               sender.accessPointNode,
                               receiver.accessPointNode);
            ValidateNativeRssi(channels.at(receiverBss).nativeLoss,
                               config.wifi.txPowerDbm,
                               ACCESS_POINT_TARGET_RSSI_DBM,
                               receiver.accessPointNode->GetObject<MobilityModel>(),
                               sender.accessPointNode->GetObject<MobilityModel>(),
                               receiver.bssId,
                               receiver.accessPointNode,
                               sender.accessPointNode);
        }
    }
}

} // namespace

double
GetSaturatedTcpAccessPointTargetRssiDbm()
{
    return ACCESS_POINT_TARGET_RSSI_DBM;
}

double
GetSaturatedTcpStationTargetRssiDbm(SaturatedRssiRange range)
{
    switch (range)
    {
    case SaturatedRssiRange::HIGH:
        return -41.5;
    case SaturatedRssiRange::MEDIUM:
        return -50.0;
    case SaturatedRssiRange::LOW:
        return -60.0;
    }
    throw SaturatedTcpConfigError("invalid saturated benchmark.rssi_range in topology");
}

SaturatedTcpTopology
BuildSaturatedTcpTopology(const SaturatedTcpConfig& config)
{
    ValidateSaturatedTcpConfig(config);

    auto placementLoss = CreateObject<LogDistancePropagationLossModel>();
    SaturatedTcpTopology topology{};
    topology.accessPointDistanceM =
        SolveDistanceForRssi(placementLoss, config.wifi.txPowerDbm, ACCESS_POINT_TARGET_RSSI_DBM);
    topology.stationDistanceM =
        SolveDistanceForRssi(placementLoss,
                             config.wifi.txPowerDbm,
                             GetSaturatedTcpStationTargetRssiDbm(config.benchmark.rssiRange));
    const auto accessPointPositions = BuildAccessPointTriangle(topology.accessPointDistanceM);

    std::array<SaturatedChannelState, 3> channels;
    if (config.benchmark.interferenceMode == SaturatedInterferenceMode::ISOLATED)
    {
        for (auto& channel : channels)
        {
            channel = CreateNativeChannel();
        }
    }
    else
    {
        const auto shared = CreateFilteredChannel();
        channels.fill(shared);
    }

    for (uint32_t bssIndex = 0; bssIndex < topology.bss.size(); ++bssIndex)
    {
        auto& bss = topology.bss.at(bssIndex);
        bss.bssId = bssIndex;
        bss.serverNode = CreateObject<Node>();
        bss.accessPointNode = CreateObject<Node>();
        bss.stationNodes.Create(config.benchmark.stationCountPerBss);

        InstallPosition(bss.accessPointNode, accessPointPositions.at(bssIndex));
        const auto stationPositions = BuildStationRing(accessPointPositions.at(bssIndex),
                                                       topology.stationDistanceM,
                                                       config.benchmark.stationCountPerBss,
                                                       bssIndex * PI / 6.0);
        for (uint32_t stationIndex = 0; stationIndex < bss.stationNodes.GetN(); ++stationIndex)
        {
            InstallPosition(bss.stationNodes.Get(stationIndex), stationPositions.at(stationIndex));
        }

        YansWifiPhyHelper phy;
        ConfigurePhy(phy, channels.at(bssIndex).channel, config.wifi);
        WifiHelper wifi;
        wifi.SetStandard(WIFI_STANDARD_80211ax);
        wifi.SetRemoteStationManager(config.wifi.rateManager);

        const Ssid ssid("saturated-bss-" + std::to_string(bssIndex));
        WifiMacHelper accessPointMac;
        accessPointMac.SetType("ns3::ApWifiMac", "Ssid", SsidValue(ssid));
        WifiMacHelper stationMac;
        stationMac.SetType("ns3::AccessTrackingStaWifiMac",
                           "Ssid",
                           SsidValue(ssid),
                           "ActiveProbing",
                           BooleanValue(false));

        const auto accessPointDevices = wifi.Install(phy, accessPointMac, bss.accessPointNode);
        bss.accessPointDevice = DynamicCast<WifiNetDevice>(accessPointDevices.Get(0));
        bss.stationDevices = wifi.Install(phy, stationMac, bss.stationNodes);
        bss.accessPointDevice->GetHeConfiguration()->m_bssColor = bssIndex + 1;
    }

    if (channels.at(0).filteredLoss)
    {
        RegisterFilteredRadios(topology, channels.at(0).filteredLoss);
    }
    ValidatePlacement(topology, channels, config);

    NodeContainer internetNodes;
    for (const auto& bss : topology.bss)
    {
        internetNodes.Add(bss.serverNode);
        internetNodes.Add(bss.accessPointNode);
        internetNodes.Add(bss.stationNodes);
    }
    InternetStackHelper internet;
    internet.Install(internetNodes);

    PointToPointHelper wired;
    wired.SetDeviceAttribute("DataRate", StringValue(config.tcp.wiredRate));
    wired.SetChannelAttribute("Delay", StringValue(config.tcp.wiredDelay));
    Ipv4StaticRoutingHelper staticRoutingHelper;
    for (uint32_t bssIndex = 0; bssIndex < topology.bss.size(); ++bssIndex)
    {
        auto& bss = topology.bss.at(bssIndex);
        NodeContainer wiredNodes(bss.serverNode, bss.accessPointNode);
        const auto wiredDevices = wired.Install(wiredNodes);

        Ipv4AddressHelper wiredAddresses;
        const std::string wiredNetwork = "10.2." + std::to_string(bssIndex) + ".0";
        wiredAddresses.SetBase(wiredNetwork.c_str(), "255.255.255.252");
        const auto wiredInterfaces = wiredAddresses.Assign(wiredDevices);
        bss.serverAddress = wiredInterfaces.GetAddress(0);
        const auto accessPointWiredAddress = wiredInterfaces.GetAddress(1);

        Ipv4AddressHelper wirelessAddresses;
        const std::string wirelessNetwork = "10.1." + std::to_string(bssIndex) + ".0";
        wirelessAddresses.SetBase(wirelessNetwork.c_str(), "255.255.255.0");
        NetDeviceContainer accessPointDevice(bss.accessPointDevice);
        const auto accessPointWirelessInterface = wirelessAddresses.Assign(accessPointDevice);
        const auto accessPointWirelessAddress = accessPointWirelessInterface.GetAddress(0);
        bss.stationInterfaces = wirelessAddresses.Assign(bss.stationDevices);

        bss.accessPointNode->GetObject<Ipv4>()->SetAttribute("IpForward", BooleanValue(true));
        const auto serverIpv4 = bss.serverNode->GetObject<Ipv4>();
        const int32_t serverInterface = serverIpv4->GetInterfaceForDevice(wiredDevices.Get(0));
        NS_ABORT_MSG_IF(serverInterface < 0, "saturated server wired interface was not installed");
        staticRoutingHelper.GetStaticRouting(serverIpv4)
            ->AddNetworkRouteTo(Ipv4Address(wirelessNetwork.c_str()),
                                Ipv4Mask("255.255.255.0"),
                                accessPointWiredAddress,
                                static_cast<uint32_t>(serverInterface));
        for (uint32_t stationIndex = 0; stationIndex < bss.stationNodes.GetN(); ++stationIndex)
        {
            const auto stationIpv4 = bss.stationNodes.Get(stationIndex)->GetObject<Ipv4>();
            const int32_t stationInterface =
                stationIpv4->GetInterfaceForDevice(bss.stationDevices.Get(stationIndex));
            NS_ABORT_MSG_IF(stationInterface < 0,
                            "saturated station Wi-Fi interface was not installed");
            staticRoutingHelper.GetStaticRouting(stationIpv4)
                ->SetDefaultRoute(accessPointWirelessAddress,
                                  static_cast<uint32_t>(stationInterface));
        }
        NS_LOG_INFO("BSS " << bssIndex << ": server " << Ipv4ToString(bss.serverAddress) << ", "
                           << bss.stationNodes.GetN() << " stations");
    }

    NS_LOG_INFO("Solved AP distance "
                << topology.accessPointDistanceM << " m for " << ACCESS_POINT_TARGET_RSSI_DBM
                << " dBm; station distance " << topology.stationDistanceM << " m for "
                << GetSaturatedTcpStationTargetRssiDbm(config.benchmark.rssiRange) << " dBm");
    return topology;
}

} // namespace ns3
