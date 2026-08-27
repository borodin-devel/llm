#ifndef SATURATED_TCP_DATA_TX_METRICS_H
#define SATURATED_TCP_DATA_TX_METRICS_H

#include "ns3/mac48-address.h"
#include "ns3/wifi-phy-band.h"
#include "ns3/wifi-psdu.h"
#include "ns3/wifi-tx-vector.h"

#include <compare>
#include <cstdint>
#include <map>
#include <vector>

namespace ns3
{

/** NSS/MCS identity for one fixed-invariant data transmission profile. */
struct DataTxProfileKey
{
    uint8_t nss; ///< Number of spatial streams.
    uint8_t mcs; ///< HE MCS index.

    auto operator<=>(const DataTxProfileKey&) const = default; ///< Compare profile keys.
};

/** Raw station-transmitted data totals for one NSS/MCS profile and interval. */
struct DataTxProfileAccumulator
{
    long double transmittedPsduBytes{0.0L}; ///< Attributed data PSDU bytes.
    uint64_t ppduAttemptCount{0};           ///< PPDU attempts starting in the interval.
    int64_t ppduAirtimeNs{0};               ///< Attributed complete-PPDU airtime.
    long double nominalRateBps{0.0L};       ///< Fixed-invariant nominal profile rate.

    /**
     * Merge another interval for the same profile.
     *
     * @param other Raw profile values to merge.
     * @throws std::invalid_argument if two populated nominal rates differ.
     */
    void Merge(const DataTxProfileAccumulator& other);
};

/** Ordered raw NSS/MCS profiles for one station and interval. */
using DataTxProfileMap = std::map<DataTxProfileKey, DataTxProfileAccumulator>;

/** Collect station-transmitted unicast data profiles by fixed window. */
class StationDataTxMetricRecorder
{
  public:
    /**
     * Construct a recorder for one measurement epoch.
     *
     * @param measurementStartNs Inclusive measurement start in nanoseconds.
     * @param measurementEndNs Exclusive measurement end in nanoseconds.
     * @param windowDurationNs Fixed window duration in nanoseconds.
     * @throws std::invalid_argument if a duration is non-positive or the epoch does not contain
     *         complete windows.
     */
    StationDataTxMetricRecorder(int64_t measurementStartNs,
                                int64_t measurementEndNs,
                                int64_t windowDurationNs);

    /**
     * Register one station transmitter.
     *
     * @param stationId Scenario-local station identifier.
     * @param transmitterAddress Station MAC transmitter address.
     * @throws std::invalid_argument if an identifier or address is duplicated, or the address is
     *         a group address.
     */
    void RegisterStation(uint32_t stationId, Mac48Address transmitterAddress);

    /**
     * Record one observed PPDU attempt from a registered station trace.
     *
     * Unknown station identifiers and PPDUs without qualifying data are ignored. Qualifying data
     * must use HE modulation, 80 MHz, and a 3200 ns guard interval.
     *
     * @param stationId Bound registered station identifier.
     * @param ppduStartNs PPDU start time in nanoseconds.
     * @param band PHY band used for transmission.
     * @param psduMap Actual transmitted PSDUs indexed by STA ID.
     * @param txVector Actual transmission vector.
     * @throws std::invalid_argument if SU shape, PHY invariants, duration, or interval arithmetic
     *         is invalid.
     */
    void RecordPpduAttempt(uint32_t stationId,
                           int64_t ppduStartNs,
                           WifiPhyBand band,
                           const WifiConstPsduMap& psduMap,
                           const WifiTxVector& txVector);

    /**
     * Get fixed-window profiles for one station.
     *
     * @param stationId Registered station identifier.
     * @return Profiles in chronological window order.
     * @throws std::out_of_range if the station is not registered.
     */
    const std::vector<DataTxProfileMap>& GetWindowProfiles(uint32_t stationId) const;

    /**
     * Get independently accumulated overall profiles for one station.
     *
     * @param stationId Registered station identifier.
     * @return Overall profiles for the measurement epoch.
     * @throws std::out_of_range if the station is not registered.
     */
    const DataTxProfileMap& GetOverallProfiles(uint32_t stationId) const;

  private:
    /** Raw identity and profile state for one registered station. */
    struct RegisteredStation
    {
        Mac48Address transmitterAddress;       ///< Station MAC transmitter address.
        std::vector<DataTxProfileMap> windows; ///< Chronological raw window profiles.
        DataTxProfileMap overall;              ///< Independently accumulated epoch profiles.
    };

    int64_t m_measurementStartNs; ///< Inclusive measurement start in nanoseconds.
    int64_t m_measurementEndNs;   ///< Exclusive measurement end in nanoseconds.
    int64_t m_windowDurationNs;   ///< Fixed profile window duration in nanoseconds.
    std::map<uint32_t, RegisteredStation> m_stations; ///< Registered station profile state.
};

} // namespace ns3

#endif // SATURATED_TCP_DATA_TX_METRICS_H
