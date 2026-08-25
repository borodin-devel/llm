#include "traffic-flow-monitor-internal.h"

#include <algorithm>
#include <map>
#include <numeric>

namespace ns3
{

TransmissionSummary
BuildTransmissionSummary(const TrafficFlowMonitorState& state)
{
    std::map<std::string, TransmissionSenderSummary> summariesBySender;
    for (const auto& [sourceIp, byteSamples] : state.transmittedBytesBySource)
    {
        TransmissionSenderSummary& summary = summariesBySender[sourceIp];
        summary.senderIpv4 = sourceIp;
        summary.transmittedPayloadBytes =
            std::accumulate(byteSamples.begin(), byteSamples.end(), uint64_t{0});
    }

    for (const auto& [flow, receiveTimestamps] : state.receiveTimestampsByFlow)
    {
        const auto transmitIt = state.transmitTimestampsByFlow.find(flow);
        const auto senderIt = summariesBySender.find(flow.sourceIp);
        if (transmitIt == state.transmitTimestampsByFlow.end() ||
            senderIt == summariesBySender.end())
        {
            continue;
        }

        const auto& transmitTimestamps = transmitIt->second;
        const std::size_t matchedCount =
            std::min(transmitTimestamps.size(), receiveTimestamps.size());
        for (std::size_t index = 0; index < matchedCount; ++index)
        {
            if (receiveTimestamps[index] <= transmitTimestamps[index])
            {
                continue;
            }

            TransmissionSenderSummary& summary = senderIt->second;
            ++summary.matchedPacketCount;
            summary.totalTransmissionDurationUs +=
                receiveTimestamps[index] - transmitTimestamps[index];
        }
    }

    TransmissionSummary summary;
    summary.senders.reserve(summariesBySender.size());
    for (auto& entry : summariesBySender)
    {
        TransmissionSenderSummary& senderSummary = entry.second;
        if (senderSummary.totalTransmissionDurationUs > 0)
        {
            senderSummary.effectiveThroughputMbps =
                static_cast<double>(senderSummary.transmittedPayloadBytes) * 8.0 /
                static_cast<double>(senderSummary.totalTransmissionDurationUs);
        }
        else
        {
            senderSummary.effectiveThroughputMbps = std::nullopt;
        }
        summary.senders.push_back(std::move(senderSummary));
    }
    return summary;
}

} // namespace ns3
