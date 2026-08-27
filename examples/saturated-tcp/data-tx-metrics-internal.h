#ifndef SATURATED_TCP_DATA_TX_METRICS_INTERNAL_H
#define SATURATED_TCP_DATA_TX_METRICS_INTERNAL_H

#include "data-tx-metrics.h"

#include <optional>

namespace ns3
{

/** Extracted raw contribution of one qualifying single-user data PPDU. */
struct DataTxProfileContribution
{
    DataTxProfileKey key;             ///< Actual NSS/MCS profile key.
    long double transmittedPsduBytes; ///< Qualifying transmitted PSDU bytes.
    int64_t ppduAirtimeNs;            ///< Complete PPDU duration in nanoseconds.
    long double nominalRateBps;       ///< Actual nominal user rate.
};

/**
 * Extract one qualifying fixed-invariant data PPDU contribution.
 *
 * @param transmitterAddress Registered station transmitter address.
 * @param band PHY band used for transmission.
 * @param psduMap Actual transmitted PSDUs indexed by STA ID.
 * @param txVector Actual transmission vector.
 * @return Raw profile contribution, or null when no PSDU data qualifies.
 * @throws std::invalid_argument if the SU shape, fixed PHY invariants, or calculated duration is
 *         invalid.
 */
std::optional<DataTxProfileContribution> ExtractDataTxProfileContribution(
    Mac48Address transmitterAddress,
    WifiPhyBand band,
    const WifiConstPsduMap& psduMap,
    const WifiTxVector& txVector);

} // namespace ns3

#endif // SATURATED_TCP_DATA_TX_METRICS_INTERNAL_H
