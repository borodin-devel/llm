#ifndef SATURATED_TCP_ACCESS_WAIT_TRACKER_H
#define SATURATED_TCP_ACCESS_WAIT_TRACKER_H

#include <cstdint>
#include <map>
#include <utility>
#include <vector>

namespace ns3
{

/** Half-open channel-access waiting interval in nanoseconds. */
struct AccessWaitIntervalNs
{
    int64_t startNs; ///< Inclusive interval start in nanoseconds.
    int64_t endNs;   ///< Exclusive interval end in nanoseconds.
};

/** Historical channel-access grant start in nanoseconds. */
struct AccessGrantStartNs
{
    uint8_t ac;      ///< Access category.
    uint8_t linkId;  ///< Link identifier.
    int64_t startNs; ///< Historical access-grant start in nanoseconds.
};

/** Collect and union station channel-access waiting intervals. */
class AccessWaitTracker
{
  public:
    /**
     * Construct a tracker for one measurement epoch.
     *
     * @param measurementStartNs Inclusive measurement start in nanoseconds.
     * @param measurementEndNs Exclusive measurement end in nanoseconds.
     */
    AccessWaitTracker(int64_t measurementStartNs, int64_t measurementEndNs);

    /**
     * Record a channel-access request, retaining the first pending request for the key.
     *
     * @param ac Access category.
     * @param linkId Link identifier.
     * @param requestTimeNs Request time in nanoseconds.
     */
    void NotifyRequest(uint8_t ac, uint8_t linkId, int64_t requestTimeNs);

    /**
     * Close a matching request at the historical TXOP start.
     *
     * @param ac Access category.
     * @param linkId Link identifier.
     * @param grantStartNs Historical access-grant start in nanoseconds.
     */
    void NotifyGrant(uint8_t ac, uint8_t linkId, int64_t grantStartNs);

    /**
     * Reconcile active grants, close remaining requests, and build the interval union.
     *
     * @param activeGrantStarts Historical starts for TXOPs active at the measurement end.
     */
    void Finalize(const std::vector<AccessGrantStartNs>& activeGrantStarts = {});

    /**
     * Get finalized waiting intervals clipped to the measurement epoch.
     *
     * @return Sorted, disjoint half-open intervals in nanoseconds.
     */
    const std::vector<AccessWaitIntervalNs>& GetUnionIntervals() const;

  private:
    using AccessKey = std::pair<uint8_t, uint8_t>; ///< Access category and link identifier.

    int64_t m_measurementStartNs;                   ///< Inclusive measurement start in nanoseconds.
    int64_t m_measurementEndNs;                     ///< Exclusive measurement end in nanoseconds.
    std::map<AccessKey, int64_t> m_pendingRequests; ///< First request time for each access key.
    std::map<uint8_t, std::vector<AccessWaitIntervalNs>>
        m_rawIntervalsByAc; ///< Unmerged historical intervals by access category.
    std::vector<AccessWaitIntervalNs> m_unionIntervals; ///< Final sorted interval union.
    bool m_finalized{false};                            ///< Whether finalization has run.
};

} // namespace ns3

#endif // SATURATED_TCP_ACCESS_WAIT_TRACKER_H
