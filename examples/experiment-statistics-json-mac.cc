#include "statistics/json/writer.h"

namespace ns3
{

namespace
{

void
WriteReasonsJson(JsonWriter& writer, const std::vector<MacDropReasonOutput>& reasons)
{
    writer.BeginArray();
    for (const auto& reason : reasons)
    {
        writer.BeginObject();
        writer.Key("reason_code");
        writer.Value(reason.reasonCode);
        writer.Key("drop_count");
        writer.Value(reason.dropCount);
        writer.EndObject();
    }
    writer.EndArray();
}

void
WritePeerJson(JsonWriter& writer, const MacPeerOutput& peer)
{
    writer.BeginObject();
    writer.Key("peer_node_id");
    writer.Value(peer.peerNodeId);
    writer.Key("peer_ipv4");
    writer.Value(peer.peerIpv4);
    writer.Key("estimated_transmit_event_count");
    writer.Value(peer.estimatedTransmitEventCount);
    writer.Key("estimated_transmitted_tcp_payload_bytes");
    writer.Value(peer.estimatedTransmittedTcpPayloadBytes);
    writer.Key("estimated_transmit_throughput_mbps");
    if (peer.estimatedTransmitThroughputMbps)
    {
        writer.Value(*peer.estimatedTransmitThroughputMbps);
    }
    else
    {
        writer.Null();
    }
    writer.Key("estimated_receive_event_count");
    writer.Value(peer.estimatedReceiveEventCount);
    writer.Key("estimated_received_tcp_payload_bytes");
    writer.Value(peer.estimatedReceivedTcpPayloadBytes);
    writer.Key("estimated_receive_throughput_mbps");
    if (peer.estimatedReceiveThroughputMbps)
    {
        writer.Value(*peer.estimatedReceiveThroughputMbps);
    }
    else
    {
        writer.Null();
    }
    writer.Key("mpdu_drop_count");
    writer.Value(peer.mpduDropCount);
    writer.Key("mpdu_drop_bytes");
    writer.Value(peer.mpduDropBytes);
    writer.Key("data_failure_count");
    writer.Value(peer.dataFailureCount);
    writer.Key("final_data_failure_count");
    writer.Value(peer.finalDataFailureCount);
    writer.Key("mpdu_drops_by_reason");
    WriteReasonsJson(writer, peer.mpduDropsByReason);
    writer.EndObject();
}

} // namespace

void
WriteMacDirectionJson(JsonWriter& writer, const MacDirectionOutput& direction)
{
    writer.BeginObject();
    writer.Key("estimated_transmit_event_count");
    writer.Value(direction.estimatedTransmitEventCount);
    writer.Key("estimated_transmitted_tcp_payload_bytes");
    writer.Value(direction.estimatedTransmittedTcpPayloadBytes);
    writer.Key("estimated_transmit_throughput_mbps");
    if (direction.estimatedTransmitThroughputMbps)
    {
        writer.Value(*direction.estimatedTransmitThroughputMbps);
    }
    else
    {
        writer.Null();
    }
    writer.Key("estimated_receive_event_count");
    writer.Value(direction.estimatedReceiveEventCount);
    writer.Key("estimated_received_tcp_payload_bytes");
    writer.Value(direction.estimatedReceivedTcpPayloadBytes);
    writer.Key("estimated_receive_throughput_mbps");
    if (direction.estimatedReceiveThroughputMbps)
    {
        writer.Value(*direction.estimatedReceiveThroughputMbps);
    }
    else
    {
        writer.Null();
    }
    writer.Key("transmit_drop_count");
    writer.Value(direction.transmitDropCount);
    writer.Key("transmit_drop_packet_bytes");
    writer.Value(direction.transmitDropPacketBytes);
    writer.Key("mpdu_drop_count");
    writer.Value(direction.mpduDropCount);
    writer.Key("mpdu_drop_bytes");
    writer.Value(direction.mpduDropBytes);
    writer.Key("data_failure_count");
    writer.Value(direction.dataFailureCount);
    writer.Key("final_data_failure_count");
    writer.Value(direction.finalDataFailureCount);
    writer.Key("mpdu_drops_by_reason");
    WriteReasonsJson(writer, direction.mpduDropsByReason);
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
