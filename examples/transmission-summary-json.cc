#include "experiment-output-internal.h"

#include <ostream>

namespace ns3
{

void
WriteTransmissionSummaryJson(std::ostream& output, const TransmissionSummary& summary)
{
    output << "{\"senders\": [";
    bool first = true;
    for (const auto& sender : summary.senders)
    {
        output << (first ? "\n" : ",\n") << "      {\"sender_ipv4\": ";
        WriteJsonScalar(output, sender.senderIpv4);
        output << ", \"matched_packet_count\": ";
        WriteJsonScalar(output, sender.matchedPacketCount);
        output << ", \"total_transmission_duration_us\": ";
        WriteJsonScalar(output, sender.totalTransmissionDurationUs);
        output << ", \"transmitted_payload_bytes\": ";
        WriteJsonScalar(output, sender.transmittedPayloadBytes);
        output << ", \"effective_throughput_mbps\": ";
        if (sender.effectiveThroughputMbps)
        {
            WriteJsonScalar(output, *sender.effectiveThroughputMbps);
        }
        else
        {
            WriteJsonScalar(output, nullptr);
        }
        output << '}';
        first = false;
    }
    output << (first ? "" : "\n    ") << "]}";
}

} // namespace ns3
