#include "../../examples/saturated-tcp/bss-link-filter.h"
#include "../../examples/saturated-tcp/topology.h"
#include "../llm-test-suite.h"

#include "ns3/boolean.h"
#include "ns3/data-rate.h"
#include "ns3/he-configuration.h"
#include "ns3/ipv4-header.h"
#include "ns3/ipv4-routing-protocol.h"
#include "ns3/ipv4.h"
#include "ns3/mobility-model.h"
#include "ns3/nstime.h"
#include "ns3/packet.h"
#include "ns3/point-to-point-channel.h"
#include "ns3/point-to-point-net-device.h"
#include "ns3/pointer.h"
#include "ns3/propagation-loss-model.h"
#include "ns3/simulator.h"
#include "ns3/socket.h"
#include "ns3/sta-wifi-mac.h"
#include "ns3/udp-socket-factory.h"
#include "ns3/wifi-acknowledgment.h"
#include "ns3/wifi-mpdu.h"
#include "ns3/wifi-net-device.h"
#include "ns3/wifi-phy.h"
#include "ns3/wifi-protection.h"
#include "ns3/wifi-tx-parameters.h"
#include "ns3/yans-wifi-channel.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

using namespace ns3;

namespace
{

/** @return Valid one-station configuration for a selected interference mode. */
SaturatedTcpConfig
MakeTopologyConfig(SaturatedInterferenceMode interferenceMode)
{
    SaturatedTcpConfig config;
    config.benchmark.stationCountPerBss = 1;
    config.benchmark.interferenceMode = interferenceMode;
    return config;
}

/**
 * Find the dedicated point-to-point device on a node.
 *
 * @param node Node to inspect.
 * @return Point-to-point device, or null when absent.
 */
Ptr<PointToPointNetDevice>
FindPointToPointDevice(Ptr<Node> node)
{
    for (uint32_t index = 0; index < node->GetNDevices(); ++index)
    {
        if (auto device = DynamicCast<PointToPointNetDevice>(node->GetDevice(index)))
        {
            return device;
        }
    }
    return nullptr;
}

/**
 * Get one installed station Wi-Fi device.
 *
 * @param topology Built saturated topology.
 * @param bssIndex Zero-based BSS index.
 * @param stationIndex Zero-based station index.
 * @return Station Wi-Fi device.
 */
Ptr<WifiNetDevice>
GetStationDevice(const SaturatedTcpTopology& topology, uint32_t bssIndex, uint32_t stationIndex)
{
    return DynamicCast<WifiNetDevice>(topology.bss.at(bssIndex).stationDevices.Get(stationIndex));
}

/**
 * Determine whether one nonempty data MPDU requires RTS/CTS protection.
 *
 * @param device Wi-Fi device whose station manager is inspected.
 * @param recipient Peer receiving the data MPDU.
 * @return True when the live station manager requires RTS/CTS.
 */
bool
RequiresRtsCts(Ptr<WifiNetDevice> device, Mac48Address recipient)
{
    WifiMacHeader header(WIFI_MAC_DATA);
    header.SetAddr1(recipient);
    header.SetAddr2(device->GetMac()->GetAddress());
    header.SetAddr3(device->GetMac()->GetAddress());
    WifiTxParameters txParameters;
    txParameters.m_txVector.SetMode(device->GetPhy()->GetDefaultMode());
    txParameters.AddMpdu(Create<WifiMpdu>(Create<Packet>(1), header));
    txParameters.m_txDuration = NanoSeconds(1);
    return device->GetRemoteStationManager()->NeedRts(header, txParameters);
}

/**
 * Get the Yans channel attached to one Wi-Fi device.
 *
 * @param device Installed Wi-Fi device.
 * @return Attached Yans channel.
 */
Ptr<YansWifiChannel>
GetYansChannel(Ptr<WifiNetDevice> device)
{
    return DynamicCast<YansWifiChannel>(device->GetPhy()->GetChannel());
}

/**
 * Read the channel's installed propagation-loss model attribute.
 *
 * @param channel Yans channel to inspect.
 * @return Installed propagation-loss model.
 */
Ptr<PropagationLossModel>
GetPropagationLoss(Ptr<YansWifiChannel> channel)
{
    PointerValue value;
    channel->GetAttribute("PropagationLossModel", value);
    return value.Get<PropagationLossModel>();
}

/**
 * Read a point-to-point device data rate.
 *
 * @param device Device to inspect.
 * @return Configured bit rate.
 */
uint64_t
GetPointToPointRate(Ptr<PointToPointNetDevice> device)
{
    DataRateValue value;
    device->GetAttribute("DataRate", value);
    return value.Get().GetBitRate();
}

/**
 * Read a point-to-point channel delay.
 *
 * @param channel Channel to inspect.
 * @return Configured channel delay.
 */
Time
GetPointToPointDelay(Ptr<PointToPointChannel> channel)
{
    TimeValue value;
    channel->GetAttribute("Delay", value);
    return value.Get();
}

/**
 * Resolve a route without transmitting a packet.
 *
 * @param source Source node.
 * @param destination Destination IPv4 address.
 * @return True when a route is available.
 */
bool
HasRoute(Ptr<Node> source, Ipv4Address destination)
{
    auto ipv4 = source->GetObject<Ipv4>();
    Ipv4Header header;
    header.SetDestination(destination);
    Socket::SocketErrno error = Socket::ERROR_NOTERROR;
    return ipv4->GetRoutingProtocol()->RouteOutput(Create<Packet>(), header, nullptr, error) !=
           nullptr;
}

/**
 * Drain received UDP packets and count them.
 *
 * @param count Packet counter.
 * @param socket Receiving socket.
 */
void
ReceivePackets(uint32_t* count, Ptr<Socket> socket)
{
    while (socket->Recv())
    {
        ++*count;
    }
}

/**
 * Send one UDP probe.
 *
 * @param socket Connected source socket.
 */
void
SendProbe(Ptr<Socket> socket)
{
    socket->Send(Create<Packet>(64));
}

/**
 * Install one connected UDP source and bound destination.
 *
 * @param source Source node.
 * @param sourceAddress Source address used for deterministic binding.
 * @param destination Destination node.
 * @param destinationAddress Destination address.
 * @param port Destination UDP port.
 * @param sendTime Time at which the probe is sent.
 * @param receiveCount Packet counter.
 * @param sockets Retained sockets.
 */
void
InstallUdpProbe(Ptr<Node> source,
                Ipv4Address sourceAddress,
                Ptr<Node> destination,
                Ipv4Address destinationAddress,
                uint16_t port,
                Time sendTime,
                uint32_t* receiveCount,
                std::vector<Ptr<Socket>>& sockets)
{
    auto sink = Socket::CreateSocket(destination, UdpSocketFactory::GetTypeId());
    const int bindSink = sink->Bind(InetSocketAddress(destinationAddress, port));
    NS_ABORT_MSG_IF(bindSink != 0, "failed to bind saturated topology UDP probe sink");
    sink->SetRecvCallback(MakeBoundCallback(&ReceivePackets, receiveCount));

    auto sender = Socket::CreateSocket(source, UdpSocketFactory::GetTypeId());
    const int bindSender = sender->Bind(InetSocketAddress(sourceAddress, 0));
    NS_ABORT_MSG_IF(bindSender != 0, "failed to bind saturated topology UDP probe source");
    const int connect = sender->Connect(InetSocketAddress(destinationAddress, port));
    NS_ABORT_MSG_IF(connect != 0, "failed to connect saturated topology UDP probe source");
    Simulator::Schedule(sendTime, &SendProbe, sender);
    sockets.push_back(sink);
    sockets.push_back(sender);
}

/**
 * @ingroup tests
 *
 * Verify the exact three-BSS node, link, PHY, manager, MAC, and color contract.
 */
class SaturatedTcpTopologyAttributesTestCase : public TestCase
{
  public:
    /** Construct the exact topology-attribute test. */
    SaturatedTcpTopologyAttributesTestCase();

  private:
    void DoRun() override;
};

SaturatedTcpTopologyAttributesTestCase::SaturatedTcpTopologyAttributesTestCase()
    : TestCase("saturated TCP exact routed three-BSS topology attributes")
{
}

void
SaturatedTcpTopologyAttributesTestCase::DoRun()
{
    const auto config = MakeTopologyConfig(SaturatedInterferenceMode::ISOLATED);
    const auto topology = BuildSaturatedTcpTopology(config);

    std::set<Ptr<Node>> servers;
    std::set<Ptr<Node>> accessPoints;
    std::set<Ptr<Node>> stations;
    std::set<Ptr<PointToPointChannel>> wiredChannels;
    for (uint32_t bssIndex = 0; bssIndex < topology.bss.size(); ++bssIndex)
    {
        const auto& bss = topology.bss.at(bssIndex);
        NS_TEST_ASSERT_MSG_EQ(bss.bssId, bssIndex, "BSS identity is not deterministic");
        NS_TEST_ASSERT_MSG_NE(bss.serverNode, nullptr, "Dedicated server node is missing");
        NS_TEST_ASSERT_MSG_NE(bss.accessPointNode, nullptr, "Access-point node is missing");
        NS_TEST_ASSERT_MSG_EQ(bss.stationNodes.GetN(), 1, "Wrong one-station fixture size");
        NS_TEST_ASSERT_MSG_EQ(bss.stationDevices.GetN(), 1, "Wrong station device count");
        NS_TEST_ASSERT_MSG_EQ(bss.stationInterfaces.GetN(), 1, "Wrong station address count");
        NS_TEST_ASSERT_MSG_EQ(servers.insert(bss.serverNode).second,
                              true,
                              "A dedicated server was shared by two BSSs");
        NS_TEST_ASSERT_MSG_EQ(accessPoints.insert(bss.accessPointNode).second,
                              true,
                              "An access point was shared by two BSSs");
        NS_TEST_ASSERT_MSG_EQ(stations.insert(bss.stationNodes.Get(0)).second,
                              true,
                              "A station was shared by two BSSs");

        const auto serverP2p = FindPointToPointDevice(bss.serverNode);
        const auto accessPointP2p = FindPointToPointDevice(bss.accessPointNode);
        NS_TEST_ASSERT_MSG_NE(serverP2p, nullptr, "Server has no dedicated wired device");
        NS_TEST_ASSERT_MSG_NE(accessPointP2p, nullptr, "AP has no dedicated wired device");
        NS_TEST_ASSERT_MSG_EQ(serverP2p->GetChannel(),
                              accessPointP2p->GetChannel(),
                              "Server and AP do not share their dedicated link");
        const auto wiredChannel = DynamicCast<PointToPointChannel>(serverP2p->GetChannel());
        NS_TEST_ASSERT_MSG_NE(wiredChannel, nullptr, "Dedicated link is not point-to-point");
        NS_TEST_ASSERT_MSG_EQ(wiredChannels.insert(wiredChannel).second,
                              true,
                              "Two BSSs shared one wired channel");
        NS_TEST_ASSERT_MSG_EQ(GetPointToPointRate(serverP2p),
                              10'000'000'000ULL,
                              "Server wired rate is not 10 Gbps");
        NS_TEST_ASSERT_MSG_EQ(GetPointToPointRate(accessPointP2p),
                              10'000'000'000ULL,
                              "AP wired rate is not 10 Gbps");
        NS_TEST_ASSERT_MSG_EQ(GetPointToPointDelay(wiredChannel),
                              MicroSeconds(100),
                              "Dedicated wired delay is not 0.1 ms");

        const auto stationDevice = GetStationDevice(topology, bssIndex, 0);
        NS_TEST_ASSERT_MSG_NE(stationDevice, nullptr, "Station device is not Wi-Fi");
        NS_TEST_ASSERT_MSG_EQ(stationDevice->GetMac()->GetInstanceTypeId().GetName(),
                              "ns3::StaWifiMac",
                              "Station does not use ordinary StaWifiMac");
        const std::array<Ptr<WifiNetDevice>, 2> wifiDevices{bss.accessPointDevice, stationDevice};
        for (const auto& device : wifiDevices)
        {
            NS_TEST_ASSERT_MSG_NE(device, nullptr, "AP or station Wi-Fi device is missing");
            const auto phy = device->GetPhy();
            NS_TEST_ASSERT_MSG_EQ(phy->GetStandard(),
                                  WIFI_STANDARD_80211ax,
                                  "Wi-Fi standard is not 802.11ax");
            NS_TEST_ASSERT_MSG_EQ(phy->GetPhyBand(),
                                  WIFI_PHY_BAND_5GHZ,
                                  "Wi-Fi PHY band is not 5 GHz");
            NS_TEST_ASSERT_MSG_EQ(phy->GetOperatingChannel().GetNumber(),
                                  42,
                                  "Wi-Fi channel number is not 42");
            NS_TEST_ASSERT_MSG_EQ(phy->GetChannelWidth(),
                                  MHz_u{80},
                                  "Wi-Fi channel width is not 80 MHz");
            NS_TEST_ASSERT_MSG_EQ(phy->GetPrimary20Index(), 0, "Primary 20 MHz index is not zero");
            NS_TEST_ASSERT_MSG_EQ_TOL(phy->GetTxPowerStart(),
                                      20.0,
                                      1e-12,
                                      "TX power start is not 20 dBm");
            NS_TEST_ASSERT_MSG_EQ_TOL(phy->GetTxPowerEnd(),
                                      20.0,
                                      1e-12,
                                      "TX power end is not 20 dBm");
            NS_TEST_ASSERT_MSG_EQ(phy->GetNTxPowerLevels(),
                                  1,
                                  "PHY exposes more than one TX power level");
            NS_TEST_ASSERT_MSG_EQ(phy->GetNumberOfAntennas(), 2, "PHY is not 2x2");
            NS_TEST_ASSERT_MSG_EQ(phy->GetMaxSupportedTxSpatialStreams(),
                                  2,
                                  "PHY does not support two TX streams");
            NS_TEST_ASSERT_MSG_EQ(phy->GetMaxSupportedRxSpatialStreams(),
                                  2,
                                  "PHY does not support two RX streams");
            NS_TEST_ASSERT_MSG_EQ(device->GetRemoteStationManager()->GetInstanceTypeId().GetName(),
                                  "ns3::MinstrelHtWifiManager",
                                  "Wrong Wi-Fi rate manager");
            NS_TEST_ASSERT_MSG_EQ(device->GetHeConfiguration()->GetGuardInterval(),
                                  NanoSeconds(3200),
                                  "HE guard interval is not 3200 ns");
        }
        NS_TEST_ASSERT_MSG_EQ(
            RequiresRtsCts(bss.accessPointDevice, stationDevice->GetMac()->GetAddress()),
            true,
            "AP station manager does not require RTS/CTS for data");
        NS_TEST_ASSERT_MSG_EQ(
            RequiresRtsCts(stationDevice, bss.accessPointDevice->GetMac()->GetAddress()),
            true,
            "STA station manager does not require RTS/CTS for data");
        NS_TEST_ASSERT_MSG_EQ(bss.accessPointDevice->GetHeConfiguration()->m_bssColor,
                              bssIndex + 1,
                              "AP BSS color is not 1, 2, or 3");
        BooleanValue forwarding;
        bss.accessPointNode->GetObject<Ipv4>()->GetAttribute("IpForward", forwarding);
        NS_TEST_ASSERT_MSG_EQ(forwarding.Get(), true, "AP IPv4 forwarding is disabled");
        NS_TEST_ASSERT_MSG_NE(bss.serverAddress,
                              Ipv4Address::GetAny(),
                              "Server address was not assigned");
        NS_TEST_ASSERT_MSG_NE(bss.stationInterfaces.GetAddress(0),
                              Ipv4Address::GetAny(),
                              "Station address was not assigned");
    }

    NS_TEST_ASSERT_MSG_EQ(servers.size(), 3, "Topology does not have exactly three servers");
    NS_TEST_ASSERT_MSG_EQ(accessPoints.size(), 3, "Topology does not have exactly three APs");
    NS_TEST_ASSERT_MSG_EQ(stations.size(), 3, "Topology does not have exactly three STAs");
    NS_TEST_ASSERT_MSG_GT(topology.accessPointDistanceM,
                          0.0,
                          "AP distance was not solved from native loss");
    NS_TEST_ASSERT_MSG_GT(topology.stationDistanceM,
                          0.0,
                          "STA distance was not solved from native loss");

    auto unsupported = config;
    unsupported.benchmark.mimoMode = SaturatedMimoMode::MU;
    bool rejected = false;
    std::string diagnostic;
    try
    {
        static_cast<void>(BuildSaturatedTcpTopology(unsupported));
    }
    catch (const std::exception& error)
    {
        rejected = true;
        diagnostic = error.what();
    }
    NS_TEST_ASSERT_MSG_EQ(rejected, true, "Topology accepted unsupported MU mode");
    NS_TEST_ASSERT_MSG_NE(diagnostic.find("DL MU-MIMO is not supported"),
                          std::string::npos,
                          "MU rejection omitted the capability diagnostic");
    Simulator::Destroy();
}

/**
 * @ingroup tests
 *
 * Verify isolated channels and the live native desired-link placement.
 */
class SaturatedTcpIsolatedTopologyTestCase : public TestCase
{
  public:
    /** Construct the isolated-channel test. */
    SaturatedTcpIsolatedTopologyTestCase();

  private:
    void DoRun() override;
};

SaturatedTcpIsolatedTopologyTestCase::SaturatedTcpIsolatedTopologyTestCase()
    : TestCase("saturated TCP isolated BSS channels and native RSSI")
{
}

void
SaturatedTcpIsolatedTopologyTestCase::DoRun()
{
    const auto config = MakeTopologyConfig(SaturatedInterferenceMode::ISOLATED);
    const auto topology = BuildSaturatedTcpTopology(config);
    std::set<Ptr<YansWifiChannel>> channels;
    auto nativeLoss = CreateObject<LogDistancePropagationLossModel>();
    for (uint32_t bssIndex = 0; bssIndex < topology.bss.size(); ++bssIndex)
    {
        const auto& bss = topology.bss.at(bssIndex);
        const auto channel = GetYansChannel(bss.accessPointDevice);
        NS_TEST_ASSERT_MSG_NE(channel, nullptr, "AP does not use a Yans channel");
        NS_TEST_ASSERT_MSG_EQ(channels.insert(channel).second,
                              true,
                              "Isolated BSSs share a channel pointer");
        NS_TEST_ASSERT_MSG_EQ(channel->GetNDevices(),
                              2,
                              "Isolated channel contains a cross-BSS radio");
        NS_TEST_ASSERT_MSG_EQ(GetYansChannel(GetStationDevice(topology, bssIndex, 0)),
                              channel,
                              "Same-BSS AP and STA do not share a channel");
        NS_TEST_ASSERT_MSG_NE(
            DynamicCast<LogDistancePropagationLossModel>(GetPropagationLoss(channel)),
            nullptr,
            "Isolated channel does not use native LogDistance loss");

        const auto apMobility = bss.accessPointNode->GetObject<MobilityModel>();
        const auto stationMobility = bss.stationNodes.Get(0)->GetObject<MobilityModel>();
        const double desiredRssi =
            nativeLoss->CalcRxPower(config.wifi.txPowerDbm, apMobility, stationMobility);
        NS_TEST_ASSERT_MSG_EQ_TOL(desiredRssi,
                                  -41.5,
                                  0.5,
                                  "Same-BSS native RSSI missed the high target");
    }
    NS_TEST_ASSERT_MSG_EQ(channels.size(), 3, "Isolated mode did not build three channels");

    const auto foreignChannel = GetYansChannel(GetStationDevice(topology, 1, 0));
    NS_TEST_ASSERT_MSG_EQ(foreignChannel == GetYansChannel(topology.bss.at(0).accessPointDevice),
                          false,
                          "Cross-BSS station delivery is possible through a shared channel");
    Simulator::Destroy();
}

/**
 * @ingroup tests
 *
 * Verify the shared wrapper's complete link matrix and routed live delivery.
 */
class SaturatedTcpCochannelTopologyTestCase : public TestCase
{
  public:
    /** Construct the co-channel propagation and routing test. */
    SaturatedTcpCochannelTopologyTestCase();

  private:
    void DoRun() override;
};

SaturatedTcpCochannelTopologyTestCase::SaturatedTcpCochannelTopologyTestCase()
    : TestCase("saturated TCP AP-only co-channel visibility and routed delivery")
{
}

void
SaturatedTcpCochannelTopologyTestCase::DoRun()
{
    const auto config = MakeTopologyConfig(SaturatedInterferenceMode::AP_ONLY_COCHANNEL);
    const auto topology = BuildSaturatedTcpTopology(config);
    const auto channel = GetYansChannel(topology.bss.at(0).accessPointDevice);
    NS_TEST_ASSERT_MSG_NE(channel, nullptr, "Shared Yans channel is missing");
    NS_TEST_ASSERT_MSG_EQ(channel->GetNDevices(),
                          6,
                          "Shared channel does not contain all AP and station radios");
    for (uint32_t bssIndex = 0; bssIndex < topology.bss.size(); ++bssIndex)
    {
        NS_TEST_ASSERT_MSG_EQ(GetYansChannel(topology.bss.at(bssIndex).accessPointDevice),
                              channel,
                              "APs do not share one co-channel object");
        NS_TEST_ASSERT_MSG_EQ(GetYansChannel(GetStationDevice(topology, bssIndex, 0)),
                              channel,
                              "Stations do not use the co-channel object");
    }

    const auto filter = DynamicCast<BssLinkFilterPropagationLossModel>(GetPropagationLoss(channel));
    NS_TEST_ASSERT_MSG_NE(filter, nullptr, "Shared channel does not use the BSS link filter");

    for (uint32_t senderBss = 0; senderBss < topology.bss.size(); ++senderBss)
    {
        const auto senderAp =
            topology.bss.at(senderBss).accessPointNode->GetObject<MobilityModel>();
        const auto senderSta =
            topology.bss.at(senderBss).stationNodes.Get(0)->GetObject<MobilityModel>();
        const auto sameBssRssi = filter->CalcRxPower(config.wifi.txPowerDbm, senderAp, senderSta);
        NS_TEST_ASSERT_MSG_EQ_TOL(sameBssRssi,
                                  -41.5,
                                  0.5,
                                  "Allowed same-BSS AP/STA link missed target RSSI");

        for (uint32_t receiverBss = 0; receiverBss < topology.bss.size(); ++receiverBss)
        {
            if (senderBss == receiverBss)
            {
                continue;
            }
            const auto receiverAp =
                topology.bss.at(receiverBss).accessPointNode->GetObject<MobilityModel>();
            const auto receiverSta =
                topology.bss.at(receiverBss).stationNodes.Get(0)->GetObject<MobilityModel>();
            NS_TEST_ASSERT_MSG_EQ_TOL(
                filter->CalcRxPower(config.wifi.txPowerDbm, senderAp, receiverAp),
                -50.0,
                0.5,
                "Allowed cross-BSS AP/AP link missed native RSSI");
            NS_TEST_ASSERT_MSG_EQ(
                std::isinf(filter->CalcRxPower(config.wifi.txPowerDbm, senderAp, receiverSta)),
                true,
                "Cross-BSS AP/STA link was not blocked");
            NS_TEST_ASSERT_MSG_EQ(
                std::isinf(filter->CalcRxPower(config.wifi.txPowerDbm, senderSta, receiverAp)),
                true,
                "Cross-BSS STA/AP link was not blocked");
            NS_TEST_ASSERT_MSG_EQ(
                std::isinf(filter->CalcRxPower(config.wifi.txPowerDbm, senderSta, receiverSta)),
                true,
                "Cross-BSS STA/STA link was not blocked");
        }
    }

    std::array<uint32_t, 3> uplinkReceived{};
    std::array<uint32_t, 3> downlinkReceived{};
    std::vector<Ptr<Socket>> sockets;
    for (uint32_t bssIndex = 0; bssIndex < topology.bss.size(); ++bssIndex)
    {
        const auto& bss = topology.bss.at(bssIndex);
        const auto stationAddress = bss.stationInterfaces.GetAddress(0);
        NS_TEST_ASSERT_MSG_EQ(HasRoute(bss.stationNodes.Get(0), bss.serverAddress),
                              true,
                              "Station has no route to its dedicated server");
        NS_TEST_ASSERT_MSG_EQ(HasRoute(bss.serverNode, stationAddress),
                              true,
                              "Server has no route to its station");
        InstallUdpProbe(bss.stationNodes.Get(0),
                        stationAddress,
                        bss.serverNode,
                        bss.serverAddress,
                        static_cast<uint16_t>(30000 + bssIndex),
                        Seconds(1.0),
                        &uplinkReceived.at(bssIndex),
                        sockets);
        InstallUdpProbe(bss.serverNode,
                        bss.serverAddress,
                        bss.stationNodes.Get(0),
                        stationAddress,
                        static_cast<uint16_t>(31000 + bssIndex),
                        Seconds(1.2),
                        &downlinkReceived.at(bssIndex),
                        sockets);
    }
    Simulator::Stop(Seconds(2));
    Simulator::Run();
    for (uint32_t bssIndex = 0; bssIndex < topology.bss.size(); ++bssIndex)
    {
        NS_TEST_ASSERT_MSG_GT(uplinkReceived.at(bssIndex),
                              0,
                              "Routed STA-to-server UDP delivery failed");
        NS_TEST_ASSERT_MSG_GT(downlinkReceived.at(bssIndex),
                              0,
                              "Routed server-to-STA UDP delivery failed");
        const auto stationMac =
            DynamicCast<StaWifiMac>(GetStationDevice(topology, bssIndex, 0)->GetMac());
        NS_TEST_ASSERT_MSG_EQ(stationMac->IsAssociated(),
                              true,
                              "Station did not associate with its own AP");
    }
    Simulator::Destroy();
}

} // namespace

std::vector<TestCase*>
CreateSaturatedTcpTopologyTestCases()
{
    return {
        new SaturatedTcpTopologyAttributesTestCase(),
        new SaturatedTcpIsolatedTopologyTestCase(),
        new SaturatedTcpCochannelTopologyTestCase(),
    };
}
