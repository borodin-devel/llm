#include "writer.h"

namespace ns3
{

namespace
{

void
WriteAgentJson(JsonWriter& writer, const AppAgentOutput& agent)
{
    writer.BeginObject();
    writer.Key("agent_key");
    writer.Value(agent.agentKey);
    writer.Key("accepted_send_count");
    writer.Value(agent.acceptedSendCount);
    writer.Key("accepted_payload_bytes");
    writer.Value(agent.acceptedPayloadBytes);
    writer.Key("accepted_throughput_mbps");
    if (agent.acceptedThroughputMbps)
    {
        writer.Value(*agent.acceptedThroughputMbps);
    }
    else
    {
        writer.Null();
    }
    writer.Key("accepted_bandwidth_share_percent");
    if (agent.acceptedBandwidthSharePercent)
    {
        writer.Value(*agent.acceptedBandwidthSharePercent);
    }
    else
    {
        writer.Null();
    }
    writer.Key("drop_event_count");
    writer.Value(agent.dropEventCount);
    writer.Key("dropped_payload_bytes");
    writer.Value(agent.droppedPayloadBytes);
    writer.EndObject();
}

void
WritePeerJson(JsonWriter& writer, const AppPeerOutput& peer)
{
    writer.BeginObject();
    writer.Key("peer_node_id");
    writer.Value(peer.peerNodeId);
    writer.Key("peer_ipv4");
    writer.Value(peer.peerIpv4);
    writer.Key("accepted_send_count");
    writer.Value(peer.acceptedSendCount);
    writer.Key("accepted_payload_bytes");
    writer.Value(peer.acceptedPayloadBytes);
    writer.Key("accepted_throughput_mbps");
    if (peer.acceptedThroughputMbps)
    {
        writer.Value(*peer.acceptedThroughputMbps);
    }
    else
    {
        writer.Null();
    }
    writer.Key("accepted_bandwidth_share_percent");
    if (peer.acceptedBandwidthSharePercent)
    {
        writer.Value(*peer.acceptedBandwidthSharePercent);
    }
    else
    {
        writer.Null();
    }
    writer.Key("receive_event_count");
    writer.Value(peer.receiveEventCount);
    writer.Key("received_payload_bytes");
    writer.Value(peer.receivedPayloadBytes);
    writer.Key("received_throughput_mbps");
    if (peer.receivedThroughputMbps)
    {
        writer.Value(*peer.receivedThroughputMbps);
    }
    else
    {
        writer.Null();
    }
    writer.Key("received_bandwidth_share_percent");
    if (peer.receivedBandwidthSharePercent)
    {
        writer.Value(*peer.receivedBandwidthSharePercent);
    }
    else
    {
        writer.Null();
    }
    writer.Key("drop_event_count");
    writer.Value(peer.dropEventCount);
    writer.Key("dropped_payload_bytes");
    writer.Value(peer.droppedPayloadBytes);
    writer.EndObject();
}

} // namespace

void
WriteAppDirectionJson(JsonWriter& writer, const AppDirectionOutput& direction)
{
    writer.BeginObject();
    writer.Key("accepted_send_count");
    writer.Value(direction.acceptedSendCount);
    writer.Key("accepted_payload_bytes");
    writer.Value(direction.acceptedPayloadBytes);
    writer.Key("accepted_throughput_mbps");
    if (direction.acceptedThroughputMbps)
    {
        writer.Value(*direction.acceptedThroughputMbps);
    }
    else
    {
        writer.Null();
    }
    writer.Key("receive_event_count");
    writer.Value(direction.receiveEventCount);
    writer.Key("received_payload_bytes");
    writer.Value(direction.receivedPayloadBytes);
    writer.Key("received_throughput_mbps");
    if (direction.receivedThroughputMbps)
    {
        writer.Value(*direction.receivedThroughputMbps);
    }
    else
    {
        writer.Null();
    }
    writer.Key("drop_event_count");
    writer.Value(direction.dropEventCount);
    writer.Key("dropped_payload_bytes");
    writer.Value(direction.droppedPayloadBytes);
    writer.Key("receive_interarrival_time");
    WriteSampleDistributionJson(writer, direction.receiveInterArrivalTime);
    writer.Key("agents");
    writer.BeginArray();
    for (const auto& agent : direction.agents)
    {
        WriteAgentJson(writer, agent);
    }
    writer.EndArray();
    writer.Key("peers");
    writer.BeginArray();
    for (const auto& peer : direction.peers)
    {
        WritePeerJson(writer, peer);
    }
    writer.EndArray();
    writer.EndObject();
}

} // namespace ns3
