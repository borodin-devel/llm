#include "traffic.h"

#include "readiness-barrier.h"
#include "saturated-tcp-sender.h"

#include "ns3/address.h"
#include "ns3/application-container.h"
#include "ns3/inet-socket-address.h"
#include "ns3/node.h"
#include "ns3/nstime.h"
#include "ns3/packet-sink-helper.h"
#include "ns3/packet-sink.h"
#include "ns3/uinteger.h"

namespace ns3
{

namespace
{

constexpr uint16_t FIRST_SOURCE_PORT = 10000;
constexpr uint16_t FIRST_DESTINATION_PORT = 20000;

/**
 * Install one independently bound sender/sink pair.
 *
 * @param bssIndex Zero-based BSS index.
 * @param stationIndex Zero-based station index.
 * @param direction Flow direction.
 * @param source Source node and address.
 * @param destination Destination node and address.
 * @param sourcePort Dedicated source port.
 * @param destinationPort Dedicated destination port.
 * @param sendSize Application packet size in bytes.
 * @param barrier Common readiness barrier.
 * @return Installed flow metadata and applications.
 */
SaturatedTcpFlow
InstallFlow(uint32_t bssIndex,
            uint32_t stationIndex,
            SaturatedTcpFlowDirection direction,
            const SaturatedTcpEndpoint& source,
            const SaturatedTcpEndpoint& destination,
            uint16_t sourcePort,
            uint16_t destinationPort,
            uint32_t sendSize,
            SaturatedReadinessBarrier& barrier)
{
    NS_ABORT_MSG_IF(!source.node || !destination.node,
                    "cannot install saturated TCP flow on a null node");

    PacketSinkHelper sinkHelper("ns3::TcpSocketFactory",
                                InetSocketAddress(destination.address, destinationPort));
    const ApplicationContainer sinkApplications = sinkHelper.Install(destination.node);
    auto sink = DynamicCast<PacketSink>(sinkApplications.Get(0));
    sink->SetStartTime(Seconds(0));

    auto sender = CreateObject<SaturatedTcpSender>();
    sender->SetAttribute("Local", AddressValue(InetSocketAddress(source.address, sourcePort)));
    sender->SetRemote(InetSocketAddress(destination.address, destinationPort));
    sender->SetAttribute("SendSize", UintegerValue(sendSize));
    source.node->AddApplication(sender);
    sender->SetStartTime(NanoSeconds(1));

    const Callback<void> readyCallback =
        barrier.RegisterSender(sender,
                               MakeCallback(&SaturatedTcpSender::StartTraffic, sender),
                               MakeCallback(&SaturatedTcpSender::StopTraffic, sender));
    sender->SetReadyCallback(readyCallback);
    barrier.RegisterApplication(sink);

    return {bssIndex,
            stationIndex,
            direction,
            sender,
            sink,
            source.address,
            destination.address,
            sourcePort,
            destinationPort};
}

} // namespace

SaturatedTcpTrafficInstallation
InstallSaturatedTcpTraffic(const std::array<SaturatedTcpBssEndpoints, 3>& endpoints,
                           const SaturatedTcpConfig& config,
                           SaturatedReadinessBarrier& barrier)
{
    SaturatedTcpTrafficInstallation installation;
    uint32_t flowIndex = 0;
    for (uint32_t bssIndex = 0; bssIndex < endpoints.size(); ++bssIndex)
    {
        const auto& bss = endpoints[bssIndex];
        NS_ABORT_MSG_IF(bss.stations.size() != config.benchmark.stationCountPerBss,
                        "saturated TCP BSS station count does not match configuration");
        for (uint32_t stationIndex = 0; stationIndex < bss.stations.size(); ++stationIndex)
        {
            const auto installDirection = [&](SaturatedTcpFlowDirection direction,
                                              const SaturatedTcpEndpoint& source,
                                              const SaturatedTcpEndpoint& destination) {
                const auto sourcePort = static_cast<uint16_t>(FIRST_SOURCE_PORT + flowIndex);
                const auto destinationPort =
                    static_cast<uint16_t>(FIRST_DESTINATION_PORT + flowIndex);
                installation.flows.push_back(InstallFlow(bssIndex,
                                                         stationIndex,
                                                         direction,
                                                         source,
                                                         destination,
                                                         sourcePort,
                                                         destinationPort,
                                                         config.tcp.segmentSizeBytes,
                                                         barrier));
                ++flowIndex;
            };

            if (config.benchmark.trafficMode == SaturatedTrafficMode::UL ||
                config.benchmark.trafficMode == SaturatedTrafficMode::UL_DL)
            {
                installDirection(SaturatedTcpFlowDirection::UL,
                                 bss.stations[stationIndex],
                                 bss.server);
            }
            if (config.benchmark.trafficMode == SaturatedTrafficMode::DL ||
                config.benchmark.trafficMode == SaturatedTrafficMode::UL_DL)
            {
                installDirection(SaturatedTcpFlowDirection::DL,
                                 bss.server,
                                 bss.stations[stationIndex]);
            }
        }
    }
    return installation;
}

} // namespace ns3
