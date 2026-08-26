#include "access-wait-tracker.h"

#include <algorithm>

namespace ns3
{

AccessWaitTracker::AccessWaitTracker(int64_t measurementStartNs, int64_t measurementEndNs)
    : m_measurementStartNs(measurementStartNs),
      m_measurementEndNs(measurementEndNs)
{
}

void
AccessWaitTracker::NotifyRequest(uint8_t ac, uint8_t linkId, int64_t requestTimeNs)
{
    m_pendingRequests.try_emplace({ac, linkId}, requestTimeNs);
}

void
AccessWaitTracker::NotifyGrant(uint8_t ac, uint8_t linkId, int64_t grantStartNs)
{
    const auto pending = m_pendingRequests.find({ac, linkId});
    if (pending == m_pendingRequests.end() || grantStartNs < pending->second)
    {
        return;
    }
    m_rawIntervalsByAc[ac].push_back({pending->second, grantStartNs});
    m_pendingRequests.erase(pending);
}

void
AccessWaitTracker::Finalize()
{
    if (m_finalized)
    {
        return;
    }
    for (const auto& [key, requestTimeNs] : m_pendingRequests)
    {
        m_rawIntervalsByAc[key.first].push_back({requestTimeNs, m_measurementEndNs});
    }
    m_pendingRequests.clear();

    std::vector<AccessWaitIntervalNs> clippedIntervals;
    for (const auto& [ac, intervals] : m_rawIntervalsByAc)
    {
        static_cast<void>(ac);
        for (const auto& interval : intervals)
        {
            const AccessWaitIntervalNs clipped{
                std::max(interval.startNs, m_measurementStartNs),
                std::min(interval.endNs, m_measurementEndNs),
            };
            if (clipped.startNs < clipped.endNs)
            {
                clippedIntervals.push_back(clipped);
            }
        }
    }

    std::sort(clippedIntervals.begin(),
              clippedIntervals.end(),
              [](const auto& left, const auto& right) {
                  return std::pair{left.startNs, left.endNs} <
                         std::pair{right.startNs, right.endNs};
              });
    for (const auto& interval : clippedIntervals)
    {
        if (m_unionIntervals.empty() || interval.startNs > m_unionIntervals.back().endNs)
        {
            m_unionIntervals.push_back(interval);
            continue;
        }
        m_unionIntervals.back().endNs = std::max(m_unionIntervals.back().endNs, interval.endNs);
    }
    m_finalized = true;
}

const std::vector<AccessWaitIntervalNs>&
AccessWaitTracker::GetUnionIntervals() const
{
    return m_unionIntervals;
}

} // namespace ns3
