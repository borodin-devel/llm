#ifndef EXPERIMENT_OUTPUT_H
#define EXPERIMENT_OUTPUT_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ns3
{

/**
 * Aggregate transmission measurements for one sender.
 *
 * MAC transmit payload samples may include repeated transmission attempts.
 * The effective throughput is absent when no matched transmit/receive pair
 * has a positive duration.
 */
struct TransmissionSenderSummary
{
    std::string senderIpv4;                         ///< Sender IPv4 address.
    uint64_t matchedPacketCount{0};                  ///< Positive-duration matched packet count.
    uint64_t totalTransmissionDurationUs{0};         ///< Sum of matched transmission durations in us.
    uint64_t transmittedPayloadBytes{0};             ///< MAC transmit payload bytes, including repeats.
    std::optional<double> effectiveThroughputMbps;   ///< Effective throughput in Mbps, if measurable.
};

/** Aggregate transmission measurements for all senders. */
struct TransmissionSummary
{
    std::vector<TransmissionSenderSummary> senders; ///< Measurements ordered by sender IPv4 address.
};

} // namespace ns3

#endif // EXPERIMENT_OUTPUT_H
