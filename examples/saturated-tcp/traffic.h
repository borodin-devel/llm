#ifndef SATURATED_TCP_TRAFFIC_H
#define SATURATED_TCP_TRAFFIC_H

#include "config.h"
#include "saturated-tcp-sender.h"

#include "ns3/ipv4-address.h"
#include "ns3/node.h"
#include "ns3/packet-sink.h"
#include "ns3/ptr.h"

#include <array>
#include <cstdint>
#include <vector>

namespace ns3
{

class SaturatedReadinessBarrier;

/** Direction of one independently connected saturated TCP flow. */
enum class SaturatedTcpFlowDirection
{
    UL, ///< Station-to-server flow.
    DL, ///< Server-to-station flow.
};

/** Node and IPv4 address forming one traffic endpoint. */
struct SaturatedTcpEndpoint
{
    Ptr<Node> node;      ///< Node that owns the endpoint application.
    Ipv4Address address; ///< Unicast IPv4 address used by the flow.
};

/** Server and ordered station endpoints for one BSS. */
struct SaturatedTcpBssEndpoints
{
    SaturatedTcpEndpoint server;                ///< Dedicated wired server endpoint.
    std::vector<SaturatedTcpEndpoint> stations; ///< Ordered station endpoints.
};

/** Installed applications and endpoint identity for one TCP connection. */
struct SaturatedTcpFlow
{
    uint32_t bssIndex{0};     ///< Zero-based BSS index.
    uint32_t stationIndex{0}; ///< Zero-based station index within the BSS.
    SaturatedTcpFlowDirection direction{SaturatedTcpFlowDirection::UL}; ///< Flow direction.
    Ptr<SaturatedTcpSender> sender; ///< Unique source application and TCP connection.
    Ptr<PacketSink> sink;           ///< Unique destination listening application.
    Ipv4Address sourceAddress;      ///< Bound source IPv4 address.
    Ipv4Address destinationAddress; ///< Bound destination IPv4 address.
    uint16_t sourcePort{0};         ///< Dedicated source TCP port.
    uint16_t destinationPort{0};    ///< Dedicated destination TCP port.
};

/** Complete deterministic traffic installation for three BSSs. */
struct SaturatedTcpTrafficInstallation
{
    std::vector<SaturatedTcpFlow> flows; ///< Independent flows in deterministic installation order.
};

/**
 * Install one independent TCP connection per active station direction.
 *
 * A single configured traffic mode applies to all three BSSs. Every source and
 * sink receives a unique endpoint/port pair, and every sender is registered
 * with the common readiness barrier.
 *
 * @param endpoints Exactly three BSS endpoint groups.
 * @param config Validated saturated benchmark configuration.
 * @param barrier Common readiness and measurement barrier.
 * @return Installed flows and applications in deterministic order.
 */
SaturatedTcpTrafficInstallation InstallSaturatedTcpTraffic(
    const std::array<SaturatedTcpBssEndpoints, 3>& endpoints,
    const SaturatedTcpConfig& config,
    SaturatedReadinessBarrier& barrier);

} // namespace ns3

#endif // SATURATED_TCP_TRAFFIC_H
