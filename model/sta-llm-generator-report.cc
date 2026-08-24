#include "llm-log.h"
#include "sta-llm-generator.h"

#include "ns3/log.h"

#include <cstdint>

namespace ns3
{

static LogComponent& g_log = llm_detail::GetStaLlmGeneratorLog();

void
StaLlmGenerator::PrintPerSecondMetrics()
{
    NS_LOG_FUNCTION(this);

    NS_LOG_WARN("========== StaLlmGenerator per-second statistics ==========");

    if (m_metricsByAbsoluteSecond.empty())
    {
        NS_LOG_WARN("[Final per-second] No transmitted data");
        NS_LOG_WARN("==========================================================");
        return;
    }

    for (const auto& [second, statistics] : m_metricsByAbsoluteSecond)
    {
        if (statistics.totalBytes == 0)
        {
            continue;
        }

        const double throughputMbps = static_cast<double>(statistics.totalBytes) * 8.0 / 1e6;
        NS_LOG_WARN(
            "[Final per-second] interval=["
            << static_cast<int64_t>(second) - static_cast<int64_t>(m_experimentStartMs / 1000)
            << ","
            << static_cast<int64_t>(second + 1) - static_cast<int64_t>(m_experimentStartMs / 1000)
            << ")s"
            << " totalBytes=" << statistics.totalBytes << " throughput=" << throughputMbps
            << " Mbps"
            << " cwnd=" << statistics.lastCwnd << " bytes");

        for (const auto& [agentKey, agentBytes] : statistics.agentBytes)
        {
            const double agentThroughputMbps = static_cast<double>(agentBytes) * 8.0 / 1e6;
            const double bandwidthSharePercent =
                statistics.totalBytes > 0 ? static_cast<double>(agentBytes) /
                                                static_cast<double>(statistics.totalBytes) * 100.0
                                          : 0.0;

            NS_LOG_WARN("[Final per-second]   Agent " << agentKey << ": bytes=" << agentBytes
                                                      << " throughput=" << agentThroughputMbps
                                                      << " Mbps"
                                                      << " share=" << bandwidthSharePercent << "%");
        }
    }

    uint64_t totalBytes = 0;
    uint64_t activeSecondCount = 0;
    for (const auto& [second, statistics] : m_metricsByAbsoluteSecond)
    {
        (void)second;
        if (statistics.totalBytes == 0)
        {
            continue;
        }
        ++activeSecondCount;
        totalBytes += statistics.totalBytes;
    }

    const double totalDurationSeconds = static_cast<double>(activeSecondCount);
    const double averageThroughputMbps =
        totalDurationSeconds > 0
            ? (static_cast<double>(totalBytes) * 8.0 / 1e6) / totalDurationSeconds
            : 0.0;

    NS_LOG_WARN("[Final overall] duration=" << totalDurationSeconds << "s totalBytes=" << totalBytes
                                            << " avgThroughput=" << averageThroughputMbps
                                            << " Mbps");
    NS_LOG_WARN("==========================================================");
}

} // namespace ns3
