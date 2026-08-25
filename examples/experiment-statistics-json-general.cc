#include "experiment-output-internal.h"

#include <ostream>

namespace ns3
{

void
WriteSampleDistributionJson(std::ostream& output, const SampleDistributionOutput& distribution)
{
    output << "{\"sample_count\":";
    WriteJsonScalar(output, distribution.sampleCount);
    output << ",\"average_us\":";
    WriteJsonScalar(output, distribution.averageUs);
    output << ",\"standard_deviation_us\":";
    WriteJsonScalar(output, distribution.standardDeviationUs);
    output << ",\"minimum_us\":";
    WriteJsonScalar(output, distribution.minimumUs);
    output << ",\"maximum_us\":";
    WriteJsonScalar(output, distribution.maximumUs);
    output << '}';
}

void
WriteGeneralDirectionJson(std::ostream& output, const GeneralDirectionOutput& direction)
{
    output << "{\"estimated_transmitted_tcp_payload_bytes\":";
    WriteJsonScalar(output, direction.estimatedTransmittedTcpPayloadBytes);
    output << ",\"estimated_matched_tcp_payload_bytes\":";
    WriteJsonScalar(output, direction.estimatedMatchedTcpPayloadBytes);
    output << ",\"matched_packet_count\":";
    WriteJsonScalar(output, direction.matchedPacketCount);
    output << ",\"total_transmission_duration_us\":";
    WriteJsonScalar(output, direction.totalTransmissionDurationUs);
    output << ",\"average_transmission_duration_us\":";
    WriteJsonScalar(output, direction.averageTransmissionDurationUs);
    output << ",\"transmission_duration_standard_deviation_us\":";
    WriteJsonScalar(output, direction.transmissionDurationStandardDeviationUs);
    output << ",\"minimum_transmission_duration_us\":";
    WriteJsonScalar(output, direction.minimumTransmissionDurationUs);
    output << ",\"maximum_transmission_duration_us\":";
    WriteJsonScalar(output, direction.maximumTransmissionDurationUs);
    output << ",\"effective_throughput_mbps\":";
    WriteJsonScalar(output, direction.effectiveThroughputMbps);
    output << ",\"application_to_phy_delay\":";
    WriteSampleDistributionJson(output, direction.applicationToPhyDelay);
    output << '}';
}

} // namespace ns3
