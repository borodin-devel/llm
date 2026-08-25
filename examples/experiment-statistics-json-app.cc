#include "experiment-output-internal.h"

#include <ostream>

namespace ns3
{

namespace
{

void
WriteAgentJson(std::ostream& output, const AppAgentOutput& agent)
{
    output << "{\"agent_key\":";
    WriteJsonScalar(output, agent.agentKey);
    output << ",\"accepted_send_count\":";
    WriteJsonScalar(output, agent.acceptedSendCount);
    output << ",\"accepted_payload_bytes\":";
    WriteJsonScalar(output, agent.acceptedPayloadBytes);
    output << ",\"accepted_throughput_mbps\":";
    WriteJsonScalar(output, agent.acceptedThroughputMbps);
    output << ",\"accepted_bandwidth_share_percent\":";
    WriteJsonScalar(output, agent.acceptedBandwidthSharePercent);
    output << ",\"drop_event_count\":";
    WriteJsonScalar(output, agent.dropEventCount);
    output << ",\"dropped_payload_bytes\":";
    WriteJsonScalar(output, agent.droppedPayloadBytes);
    output << '}';
}

void
WritePeerJson(std::ostream& output, const AppPeerOutput& peer)
{
    output << "{\"peer_node_id\":";
    WriteJsonScalar(output, peer.peerNodeId);
    output << ",\"peer_ipv4\":";
    WriteJsonScalar(output, peer.peerIpv4);
    output << ",\"accepted_send_count\":";
    WriteJsonScalar(output, peer.acceptedSendCount);
    output << ",\"accepted_payload_bytes\":";
    WriteJsonScalar(output, peer.acceptedPayloadBytes);
    output << ",\"accepted_throughput_mbps\":";
    WriteJsonScalar(output, peer.acceptedThroughputMbps);
    output << ",\"accepted_bandwidth_share_percent\":";
    WriteJsonScalar(output, peer.acceptedBandwidthSharePercent);
    output << ",\"receive_event_count\":";
    WriteJsonScalar(output, peer.receiveEventCount);
    output << ",\"received_payload_bytes\":";
    WriteJsonScalar(output, peer.receivedPayloadBytes);
    output << ",\"received_throughput_mbps\":";
    WriteJsonScalar(output, peer.receivedThroughputMbps);
    output << ",\"received_bandwidth_share_percent\":";
    WriteJsonScalar(output, peer.receivedBandwidthSharePercent);
    output << ",\"drop_event_count\":";
    WriteJsonScalar(output, peer.dropEventCount);
    output << ",\"dropped_payload_bytes\":";
    WriteJsonScalar(output, peer.droppedPayloadBytes);
    output << '}';
}

} // namespace

void
WriteAppDirectionJson(std::ostream& output, const AppDirectionOutput& direction)
{
    output << "{\"accepted_send_count\":";
    WriteJsonScalar(output, direction.acceptedSendCount);
    output << ",\"accepted_payload_bytes\":";
    WriteJsonScalar(output, direction.acceptedPayloadBytes);
    output << ",\"accepted_throughput_mbps\":";
    WriteJsonScalar(output, direction.acceptedThroughputMbps);
    output << ",\"receive_event_count\":";
    WriteJsonScalar(output, direction.receiveEventCount);
    output << ",\"received_payload_bytes\":";
    WriteJsonScalar(output, direction.receivedPayloadBytes);
    output << ",\"received_throughput_mbps\":";
    WriteJsonScalar(output, direction.receivedThroughputMbps);
    output << ",\"drop_event_count\":";
    WriteJsonScalar(output, direction.dropEventCount);
    output << ",\"dropped_payload_bytes\":";
    WriteJsonScalar(output, direction.droppedPayloadBytes);
    output << ",\"receive_interarrival_time\":";
    WriteSampleDistributionJson(output, direction.receiveInterArrivalTime);
    output << ",\"agents\":[";
    bool first = true;
    for (const auto& agent : direction.agents)
    {
        output << (first ? "" : ",");
        WriteAgentJson(output, agent);
        first = false;
    }
    output << "],\"peers\":[";
    first = true;
    for (const auto& peer : direction.peers)
    {
        output << (first ? "" : ",");
        WritePeerJson(output, peer);
        first = false;
    }
    output << "]}";
}

} // namespace ns3
