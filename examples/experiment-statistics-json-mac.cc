#include "experiment-output-internal.h"

#include <ostream>

namespace ns3
{

namespace
{

void
WriteReasonsJson(std::ostream& output, const std::vector<MacDropReasonOutput>& reasons)
{
    output << '[';
    bool first = true;
    for (const auto& reason : reasons)
    {
        output << (first ? "" : ",") << "{\"reason_code\":";
        WriteJsonScalar(output, reason.reasonCode);
        output << ",\"drop_count\":";
        WriteJsonScalar(output, reason.dropCount);
        output << '}';
        first = false;
    }
    output << ']';
}

void
WritePeerJson(std::ostream& output, const MacPeerOutput& peer)
{
    output << "{\"peer_node_id\":";
    WriteJsonScalar(output, peer.peerNodeId);
    output << ",\"peer_ipv4\":";
    WriteJsonScalar(output, peer.peerIpv4);
    output << ",\"estimated_transmit_event_count\":";
    WriteJsonScalar(output, peer.estimatedTransmitEventCount);
    output << ",\"estimated_transmitted_tcp_payload_bytes\":";
    WriteJsonScalar(output, peer.estimatedTransmittedTcpPayloadBytes);
    output << ",\"estimated_transmit_throughput_mbps\":";
    WriteJsonScalar(output, peer.estimatedTransmitThroughputMbps);
    output << ",\"estimated_receive_event_count\":";
    WriteJsonScalar(output, peer.estimatedReceiveEventCount);
    output << ",\"estimated_received_tcp_payload_bytes\":";
    WriteJsonScalar(output, peer.estimatedReceivedTcpPayloadBytes);
    output << ",\"estimated_receive_throughput_mbps\":";
    WriteJsonScalar(output, peer.estimatedReceiveThroughputMbps);
    output << ",\"mpdu_drop_count\":";
    WriteJsonScalar(output, peer.mpduDropCount);
    output << ",\"mpdu_drop_bytes\":";
    WriteJsonScalar(output, peer.mpduDropBytes);
    output << ",\"data_failure_count\":";
    WriteJsonScalar(output, peer.dataFailureCount);
    output << ",\"final_data_failure_count\":";
    WriteJsonScalar(output, peer.finalDataFailureCount);
    output << ",\"mpdu_drops_by_reason\":";
    WriteReasonsJson(output, peer.mpduDropsByReason);
    output << '}';
}

} // namespace

void
WriteMacDirectionJson(std::ostream& output, const MacDirectionOutput& direction)
{
    output << "{\"estimated_transmit_event_count\":";
    WriteJsonScalar(output, direction.estimatedTransmitEventCount);
    output << ",\"estimated_transmitted_tcp_payload_bytes\":";
    WriteJsonScalar(output, direction.estimatedTransmittedTcpPayloadBytes);
    output << ",\"estimated_transmit_throughput_mbps\":";
    WriteJsonScalar(output, direction.estimatedTransmitThroughputMbps);
    output << ",\"estimated_receive_event_count\":";
    WriteJsonScalar(output, direction.estimatedReceiveEventCount);
    output << ",\"estimated_received_tcp_payload_bytes\":";
    WriteJsonScalar(output, direction.estimatedReceivedTcpPayloadBytes);
    output << ",\"estimated_receive_throughput_mbps\":";
    WriteJsonScalar(output, direction.estimatedReceiveThroughputMbps);
    output << ",\"transmit_drop_count\":";
    WriteJsonScalar(output, direction.transmitDropCount);
    output << ",\"transmit_drop_packet_bytes\":";
    WriteJsonScalar(output, direction.transmitDropPacketBytes);
    output << ",\"mpdu_drop_count\":";
    WriteJsonScalar(output, direction.mpduDropCount);
    output << ",\"mpdu_drop_bytes\":";
    WriteJsonScalar(output, direction.mpduDropBytes);
    output << ",\"data_failure_count\":";
    WriteJsonScalar(output, direction.dataFailureCount);
    output << ",\"final_data_failure_count\":";
    WriteJsonScalar(output, direction.finalDataFailureCount);
    output << ",\"mpdu_drops_by_reason\":";
    WriteReasonsJson(output, direction.mpduDropsByReason);
    output << ",\"peers\":[";
    bool first = true;
    for (const auto& peer : direction.peers)
    {
        output << (first ? "" : ",");
        WritePeerJson(output, peer);
        first = false;
    }
    output << "]}";
}

} // namespace ns3
