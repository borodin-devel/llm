#include "experiment-output-internal.h"

#include <ostream>

namespace ns3
{

namespace
{

void
WritePeerJson(std::ostream& output, const PhyPeerOutput& peer)
{
    output << "{\"peer_node_id\":";
    WriteJsonScalar(output, peer.peerNodeId);
    output << ",\"peer_ipv4\":";
    WriteJsonScalar(output, peer.peerIpv4);
    output << ",\"tagged_payload_bytes\":";
    WriteJsonScalar(output, peer.taggedPayloadBytes);
    output << ",\"unique_tagged_payload_bytes\":";
    WriteJsonScalar(output, peer.uniqueTaggedPayloadBytes);
    output << ",\"transmission_attempt_count\":";
    WriteJsonScalar(output, peer.transmissionAttemptCount);
    output << ",\"retransmission_count\":";
    WriteJsonScalar(output, peer.retransmissionCount);
    output << ",\"transmission_airtime_us\":";
    WriteJsonScalar(output, peer.transmissionAirtimeUs);
    output << ",\"average_data_rate_mbps\":";
    WriteJsonScalar(output, peer.averageDataRateMbps);
    output << ",\"throughput_mbps\":";
    WriteJsonScalar(output, peer.throughputMbps);
    output << '}';
}

void
WriteDirectionJson(std::ostream& output, const PhyDirectionOutput& direction)
{
    output << "{\"tagged_payload_bytes\":";
    WriteJsonScalar(output, direction.taggedPayloadBytes);
    output << ",\"unique_tagged_payload_bytes\":";
    WriteJsonScalar(output, direction.uniqueTaggedPayloadBytes);
    output << ",\"tagged_mpdu_count\":";
    WriteJsonScalar(output, direction.taggedMpduCount);
    output << ",\"complete_tagged_mpdu_bytes\":";
    WriteJsonScalar(output, direction.completeTaggedMpduBytes);
    output << ",\"transmission_attempt_count\":";
    WriteJsonScalar(output, direction.transmissionAttemptCount);
    output << ",\"retransmission_count\":";
    WriteJsonScalar(output, direction.retransmissionCount);
    output << ",\"transmission_airtime_us\":";
    WriteJsonScalar(output, direction.transmissionAirtimeUs);
    output << ",\"average_data_rate_mbps\":";
    WriteJsonScalar(output, direction.averageDataRateMbps);
    output << ",\"throughput_mbps\":";
    WriteJsonScalar(output, direction.throughputMbps);
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

} // namespace

void
WritePhyCategoryJson(std::ostream& output, const PhyCategoryOutput& category)
{
    output << "{\"busy_time_us\":";
    WriteJsonScalar(output, category.busyTimeUs);
    output << ",\"channel_utilization_percent\":";
    WriteJsonScalar(output, category.channelUtilizationPercent);
    output << ",\"uplink\":";
    WriteDirectionJson(output, category.uplink);
    output << ",\"downlink\":";
    WriteDirectionJson(output, category.downlink);
    output << '}';
}

} // namespace ns3
