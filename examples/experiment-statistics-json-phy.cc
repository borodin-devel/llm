#include "statistics/json/writer.h"

namespace ns3
{

namespace
{

void
WritePeerJson(JsonWriter& writer, const PhyPeerOutput& peer)
{
    writer.BeginObject();
    writer.Key("peer_node_id");
    writer.Value(peer.peerNodeId);
    writer.Key("peer_ipv4");
    writer.Value(peer.peerIpv4);
    writer.Key("tagged_payload_bytes");
    writer.Value(peer.taggedPayloadBytes);
    writer.Key("unique_tagged_payload_bytes");
    writer.Value(peer.uniqueTaggedPayloadBytes);
    writer.Key("transmission_attempt_count");
    writer.Value(peer.transmissionAttemptCount);
    writer.Key("retransmission_count");
    writer.Value(peer.retransmissionCount);
    writer.Key("transmission_airtime_us");
    writer.Value(peer.transmissionAirtimeUs);
    writer.Key("average_data_rate_mbps");
    if (peer.averageDataRateMbps)
    {
        writer.Value(*peer.averageDataRateMbps);
    }
    else
    {
        writer.Null();
    }
    writer.Key("throughput_mbps");
    if (peer.throughputMbps)
    {
        writer.Value(*peer.throughputMbps);
    }
    else
    {
        writer.Null();
    }
    writer.EndObject();
}

void
WriteDirectionJson(JsonWriter& writer, const PhyDirectionOutput& direction)
{
    writer.BeginObject();
    writer.Key("tagged_payload_bytes");
    writer.Value(direction.taggedPayloadBytes);
    writer.Key("unique_tagged_payload_bytes");
    writer.Value(direction.uniqueTaggedPayloadBytes);
    writer.Key("tagged_mpdu_count");
    writer.Value(direction.taggedMpduCount);
    writer.Key("complete_tagged_mpdu_bytes");
    writer.Value(direction.completeTaggedMpduBytes);
    writer.Key("transmission_attempt_count");
    writer.Value(direction.transmissionAttemptCount);
    writer.Key("retransmission_count");
    writer.Value(direction.retransmissionCount);
    writer.Key("transmission_airtime_us");
    writer.Value(direction.transmissionAirtimeUs);
    writer.Key("average_data_rate_mbps");
    if (direction.averageDataRateMbps)
    {
        writer.Value(*direction.averageDataRateMbps);
    }
    else
    {
        writer.Null();
    }
    writer.Key("throughput_mbps");
    if (direction.throughputMbps)
    {
        writer.Value(*direction.throughputMbps);
    }
    else
    {
        writer.Null();
    }
    writer.Key("peers");
    writer.BeginArray();
    for (const auto& peer : direction.peers)
    {
        WritePeerJson(writer, peer);
    }
    writer.EndArray();
    writer.EndObject();
}

} // namespace

void
WritePhyCategoryJson(JsonWriter& writer, const PhyCategoryOutput& category)
{
    writer.BeginObject();
    writer.Key("busy_time_us");
    writer.Value(category.busyTimeUs);
    writer.Key("channel_utilization_percent");
    if (category.channelUtilizationPercent)
    {
        writer.Value(*category.channelUtilizationPercent);
    }
    else
    {
        writer.Null();
    }
    writer.Key("uplink");
    WriteDirectionJson(writer, category.uplink);
    writer.Key("downlink");
    WriteDirectionJson(writer, category.downlink);
    writer.EndObject();
}

} // namespace ns3
