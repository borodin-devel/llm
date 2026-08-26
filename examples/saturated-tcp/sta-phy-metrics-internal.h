#ifndef SATURATED_TCP_STA_PHY_METRICS_INTERNAL_H
#define SATURATED_TCP_STA_PHY_METRICS_INTERNAL_H

#include "sta-phy-metrics.h"

#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

namespace ns3
{

/** Extracted raw contribution of one qualifying station PPDU. */
struct StationPpduMetricContribution
{
    long double nominalRateBps{0.0L}; ///< PSDU-byte-weighted actual nominal user rate.
    long double psduBits{0.0L};       ///< Qualifying PSDU bits.
    int64_t ppduAirtimeNs{0};         ///< Complete PPDU duration in nanoseconds.
};

/**
 * Extract qualifying bytes, actual per-user rates, and complete duration.
 *
 * @param transmitterAddress Registered station transmitter address.
 * @param band PHY band used for the transmission.
 * @param psduMap Actual transmitted PSDUs indexed by STA ID.
 * @param txVector Actual transmission vector, including per-user modes.
 * @return Raw PPDU contribution, or null when no PSDU qualifies.
 */
std::optional<StationPpduMetricContribution> ExtractStationPpduMetricContribution(
    Mac48Address transmitterAddress,
    WifiPhyBand band,
    const WifiConstPsduMap& psduMap,
    const WifiTxVector& txVector);

/**
 * Split a half-open interval across fixed measurement windows.
 *
 * @param intervalStartNs Inclusive absolute interval start.
 * @param intervalEndNs Exclusive absolute interval end.
 * @param measurementStartNs Inclusive measurement start.
 * @param measurementEndNs Exclusive measurement end.
 * @param windowDurationNs Fixed window duration.
 * @return Window index and exact nanosecond overlap pairs.
 */
std::vector<std::pair<std::size_t, int64_t>> SplitStationMetricInterval(int64_t intervalStartNs,
                                                                        int64_t intervalEndNs,
                                                                        int64_t measurementStartNs,
                                                                        int64_t measurementEndNs,
                                                                        int64_t windowDurationNs);

} // namespace ns3

#endif // SATURATED_TCP_STA_PHY_METRICS_INTERNAL_H
