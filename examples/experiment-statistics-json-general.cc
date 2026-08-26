#include "statistics/json/writer.h"

namespace ns3
{

void
WriteSampleDistributionJson(JsonWriter& writer, const SampleDistributionOutput& distribution)
{
    writer.BeginObject();
    writer.Key("sample_count");
    writer.Value(distribution.sampleCount);
    writer.Key("average_us");
    if (distribution.averageUs)
    {
        writer.Value(*distribution.averageUs);
    }
    else
    {
        writer.Null();
    }
    writer.Key("standard_deviation_us");
    if (distribution.standardDeviationUs)
    {
        writer.Value(*distribution.standardDeviationUs);
    }
    else
    {
        writer.Null();
    }
    writer.Key("minimum_us");
    if (distribution.minimumUs)
    {
        writer.Value(*distribution.minimumUs);
    }
    else
    {
        writer.Null();
    }
    writer.Key("maximum_us");
    if (distribution.maximumUs)
    {
        writer.Value(*distribution.maximumUs);
    }
    else
    {
        writer.Null();
    }
    writer.EndObject();
}

void
WriteGeneralDirectionJson(JsonWriter& writer, const GeneralDirectionOutput& direction)
{
    writer.BeginObject();
    writer.Key("estimated_transmitted_tcp_payload_bytes");
    writer.Value(direction.estimatedTransmittedTcpPayloadBytes);
    writer.Key("estimated_matched_tcp_payload_bytes");
    writer.Value(direction.estimatedMatchedTcpPayloadBytes);
    writer.Key("matched_packet_count");
    writer.Value(direction.matchedPacketCount);
    writer.Key("total_transmission_duration_us");
    writer.Value(direction.totalTransmissionDurationUs);
    writer.Key("average_transmission_duration_us");
    if (direction.averageTransmissionDurationUs)
    {
        writer.Value(*direction.averageTransmissionDurationUs);
    }
    else
    {
        writer.Null();
    }
    writer.Key("transmission_duration_standard_deviation_us");
    if (direction.transmissionDurationStandardDeviationUs)
    {
        writer.Value(*direction.transmissionDurationStandardDeviationUs);
    }
    else
    {
        writer.Null();
    }
    writer.Key("minimum_transmission_duration_us");
    if (direction.minimumTransmissionDurationUs)
    {
        writer.Value(*direction.minimumTransmissionDurationUs);
    }
    else
    {
        writer.Null();
    }
    writer.Key("maximum_transmission_duration_us");
    if (direction.maximumTransmissionDurationUs)
    {
        writer.Value(*direction.maximumTransmissionDurationUs);
    }
    else
    {
        writer.Null();
    }
    writer.Key("effective_throughput_mbps");
    if (direction.effectiveThroughputMbps)
    {
        writer.Value(*direction.effectiveThroughputMbps);
    }
    else
    {
        writer.Null();
    }
    writer.Key("application_to_phy_delay");
    WriteSampleDistributionJson(writer, direction.applicationToPhyDelay);
    writer.EndObject();
}

} // namespace ns3
