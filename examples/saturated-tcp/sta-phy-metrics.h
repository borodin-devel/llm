#ifndef SATURATED_TCP_STA_PHY_METRICS_H
#define SATURATED_TCP_STA_PHY_METRICS_H

#include "access-wait-tracker.h"

#include "ns3/mac48-address.h"
#include "ns3/wifi-phy-band.h"
#include "ns3/wifi-psdu.h"
#include "ns3/wifi-tx-vector.h"

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace ns3
{

/** Raw station-transmitted PHY metric totals for one interval. */
struct StationPhyMetricAccumulator
{
    long double nominalRateBpsNs{0.0L}; ///< Nominal PHY rate times PPDU airtime.
    long double psduBits{0.0L};         ///< Qualifying transmitted PSDU bits.
    int64_t ppduAirtimeNs{0};           ///< Qualifying complete PPDU airtime.
    int64_t contentionNs{0};            ///< Unioned EDCA access-wait time.

    /**
     * Add another raw interval to this accumulator.
     *
     * @param other Raw values to add.
     */
    void Merge(const StationPhyMetricAccumulator& other);
};

/** Derived station-transmitted PHY metrics for one interval. */
struct StationPhyMetricOutput
{
    std::optional<double> averageTheoreticalPhyRateMbps; ///< Airtime-weighted nominal PHY rate.
    std::optional<double>
        averagePracticalPhyRateMbps;          ///< PSDU bits divided by complete PPDU airtime.
    std::optional<double> channelEfficiency;  ///< Practical rate divided by theoretical rate.
    std::optional<double> contentionFraction; ///< EDCA union wait divided by interval duration.
};

/**
 * Derive validated station PHY metrics from raw values.
 *
 * Unit-interval results within 1e-9 of zero or one are clamped to the boundary.
 * A practical rate above its theoretical rate by more than the same relative
 * tolerance is rejected.
 *
 * @param accumulator Raw station metric totals.
 * @param windowDurationNs Metric interval duration in nanoseconds.
 * @return Derived optional metrics.
 * @throws std::invalid_argument if raw values are negative, non-finite, inconsistent, or
 *         materially outside their valid ranges.
 */
StationPhyMetricOutput DeriveStationPhyMetrics(const StationPhyMetricAccumulator& accumulator,
                                               int64_t windowDurationNs);

/** Collect raw station-transmitted PPDU and contention metrics by fixed window. */
class StationPhyMetricRecorder
{
  public:
    /**
     * Construct a recorder for one measurement epoch.
     *
     * @param measurementStartNs Inclusive measurement start in nanoseconds.
     * @param measurementEndNs Exclusive measurement end in nanoseconds.
     * @param windowDurationNs Fixed window duration in nanoseconds.
     * @throws std::invalid_argument if the epoch is empty, a duration is non-positive, or the
     *         epoch is not an exact multiple of the window duration.
     */
    StationPhyMetricRecorder(int64_t measurementStartNs,
                             int64_t measurementEndNs,
                             int64_t windowDurationNs);

    /**
     * Register one station transmitter.
     *
     * The identifier is intended to be bound only to that station device's
     * `PhyTxPsduBegin` trace. Unknown identifiers are ignored by recording methods.
     *
     * @param stationId Scenario-local station identifier.
     * @param transmitterAddress Station MAC transmitter address.
     * @throws std::invalid_argument if the identifier or address is already registered, or the
     *         address is a group address.
     */
    void RegisterStation(uint32_t stationId, Mac48Address transmitterAddress);

    /**
     * Record one qualifying PPDU attempt from a registered station trace.
     *
     * Data, TCP-ACK-sized data, unicast control frames, aggregated MPDUs, and
     * retries qualify. Data and two-address control frames must carry the registered
     * station transmitter address. One-address ACK and CTS frames rely on the
     * recorder-owned station PHY binding. Management, null-data, group-addressed,
     * mismatched-source, and unknown-transmitter observations do not qualify.
     *
     * @param stationId Bound registered station identifier.
     * @param ppduStartNs PPDU start time in nanoseconds.
     * @param band PHY band used for the transmission.
     * @param psduMap Actual transmitted PSDUs indexed by STA ID.
     * @param txVector Actual transmission vector, including per-user modes.
     * @throws std::invalid_argument if more than one PSDU is non-null in the SU-only benchmark.
     */
    void RecordPpduAttempt(uint32_t stationId,
                           int64_t ppduStartNs,
                           WifiPhyBand band,
                           const WifiConstPsduMap& psduMap,
                           const WifiTxVector& txVector);

    /**
     * Split and add finalized Task 4 access-wait union intervals.
     *
     * Intervals are normalized as a wall-clock union before they are split.
     *
     * @param stationId Bound registered station identifier.
     * @param intervals Finalized half-open waiting intervals in nanoseconds.
     * @throws std::invalid_argument if an interval ends before it starts.
     */
    void IngestContentionIntervals(uint32_t stationId,
                                   const std::vector<AccessWaitIntervalNs>& intervals);

    /**
     * Get raw fixed-window accumulators for one station.
     *
     * @param stationId Registered station identifier.
     * @return Accumulators in chronological window order.
     * @throws std::out_of_range if the station is not registered.
     */
    const std::vector<StationPhyMetricAccumulator>& GetWindowAccumulators(uint32_t stationId) const;

    /**
     * Rebuild one station's overall raw accumulator by merging its windows.
     *
     * @param stationId Registered station identifier.
     * @return Raw overall metric totals.
     * @throws std::out_of_range if the station is not registered.
     */
    StationPhyMetricAccumulator BuildOverallAccumulator(uint32_t stationId) const;

  private:
    /** Raw state and transmitter identity for one registered station. */
    struct RegisteredStation
    {
        Mac48Address transmitterAddress;                  ///< Station MAC transmitter address.
        std::vector<StationPhyMetricAccumulator> windows; ///< Raw chronological windows.
    };

    int64_t m_measurementStartNs; ///< Inclusive measurement start in nanoseconds.
    int64_t m_measurementEndNs;   ///< Exclusive measurement end in nanoseconds.
    int64_t m_windowDurationNs;   ///< Fixed metric window duration in nanoseconds.
    std::map<uint32_t, RegisteredStation> m_stations; ///< Registered station raw state.
};

} // namespace ns3

#endif // SATURATED_TCP_STA_PHY_METRICS_H
