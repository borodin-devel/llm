#ifndef SATURATED_TCP_TOPOLOGY_H
#define SATURATED_TCP_TOPOLOGY_H

#include "config.h"

#include "ns3/ipv4-address.h"
#include "ns3/ipv4-interface-container.h"
#include "ns3/net-device-container.h"
#include "ns3/node-container.h"
#include "ns3/ptr.h"

#include <array>
#include <cstdint>

namespace ns3
{

class Node;
class WifiNetDevice;

/** Nodes, radios, and addresses belonging to one saturated benchmark BSS. */
struct SaturatedTcpBssTopology
{
    uint32_t bssId;                           ///< Zero-based BSS identifier.
    Ptr<Node> serverNode;                     ///< Dedicated wired server.
    Ptr<Node> accessPointNode;                ///< Routing Wi-Fi access point.
    NodeContainer stationNodes;               ///< Ordered station nodes.
    Ptr<WifiNetDevice> accessPointDevice;     ///< Access-point Wi-Fi device.
    NetDeviceContainer stationDevices;        ///< Ordered station Wi-Fi devices.
    Ipv4InterfaceContainer stationInterfaces; ///< Ordered station IPv4 interfaces.
    Ipv4Address serverAddress;                ///< Dedicated server IPv4 endpoint.
};

/** Complete fixed three-BSS saturated benchmark topology. */
struct SaturatedTcpTopology
{
    std::array<SaturatedTcpBssTopology, 3> bss; ///< Three routed BSS groups.
    double accessPointDistanceM;                ///< Solved AP triangle side in meters.
    double stationDistanceM;                    ///< Solved AP-to-station radius in meters.
};

/**
 * Get the fixed native AP-to-AP RSSI target.
 *
 * @return AP-to-AP receive-power target in dBm.
 */
double GetSaturatedTcpAccessPointTargetRssiDbm();

/**
 * Get the desired AP/station RSSI target for a matrix range.
 *
 * @param range Configured RSSI range.
 * @return Desired-link receive-power target in dBm.
 * @throws SaturatedTcpConfigError if @p range is invalid.
 */
double GetSaturatedTcpStationTargetRssiDbm(SaturatedRssiRange range);

/**
 * Build the exact routed three-BSS saturated benchmark topology.
 *
 * Every BSS receives a dedicated 10 Gbps/0.1 ms server link, one 802.11ax AP,
 * and the configured number of 2x2 stations. Isolated mode creates three
 * native channels. AP-only co-channel mode creates one filtered channel where
 * only AP/AP signals may cross BSS boundaries.
 *
 * @param config Validated saturated benchmark configuration.
 * @return Installed nodes, devices, addresses, and solved distances.
 * @throws SaturatedTcpConfigError if the configuration is invalid.
 * @throws std::runtime_error if native RSSI validation fails.
 */
SaturatedTcpTopology BuildSaturatedTcpTopology(const SaturatedTcpConfig& config);

} // namespace ns3

#endif // SATURATED_TCP_TOPOLOGY_H
