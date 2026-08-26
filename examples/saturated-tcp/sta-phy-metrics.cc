#include "sta-phy-metrics-internal.h"

#include "ns3/wifi-mpdu.h"
#include "ns3/wifi-phy.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

namespace ns3
{

namespace
{

constexpr long double METRIC_TOLERANCE = 1e-9L; ///< Floating-point validation tolerance.

/**
 * Determine whether one MPDU belongs to the station-transmitted benchmark metric.
 *
 * @param mpdu Candidate MPDU.
 * @param transmitterAddress Registered station transmitter address.
 * @return Whether the MPDU qualifies.
 */
bool
IsQualifyingStationMpdu(const WifiMpdu& mpdu, Mac48Address transmitterAddress)
{
    const auto& header = mpdu.GetHeader();
    if (header.GetAddr1().IsGroup())
    {
        return false;
    }
    if (header.HasData())
    {
        return header.GetAddr2() == transmitterAddress;
    }
    return header.IsCtl();
}

/**
 * Count qualifying PSDU bytes, retaining aggregate delimiter and padding bytes.
 *
 * @param psdu Candidate PSDU.
 * @param transmitterAddress Registered station transmitter address.
 * @return Qualifying PSDU bytes.
 */
uint64_t
GetQualifyingPsduBytes(const WifiPsdu& psdu, Mac48Address transmitterAddress)
{
    uint64_t qualifyingMpduBytes = 0;
    std::size_t presentMpdus = 0;
    std::size_t qualifyingMpdus = 0;
    for (const auto& mpdu : psdu)
    {
        if (!mpdu)
        {
            continue;
        }
        ++presentMpdus;
        if (IsQualifyingStationMpdu(*mpdu, transmitterAddress))
        {
            ++qualifyingMpdus;
            qualifyingMpduBytes += mpdu->GetSize();
        }
    }
    if (qualifyingMpdus == 0)
    {
        return 0;
    }
    if (qualifyingMpdus == presentMpdus)
    {
        return psdu.GetSize();
    }
    return qualifyingMpduBytes;
}

/**
 * Validate and clamp one dimensionless unit-interval result.
 *
 * @param value Candidate value.
 * @param metric Diagnostic metric name.
 * @return Validated value as a double.
 * @throws std::invalid_argument if the value is non-finite or materially outside [0, 1].
 */
double
ValidateUnitInterval(long double value, const char* metric)
{
    if (!std::isfinite(value) || value < -METRIC_TOLERANCE || value > 1.0L + METRIC_TOLERANCE)
    {
        throw std::invalid_argument(std::string(metric) + " is outside [0, 1]");
    }
    return static_cast<double>(std::clamp(value, 0.0L, 1.0L));
}

/**
 * Validate raw accumulator domains and denominator consistency.
 *
 * @param accumulator Raw values to validate.
 * @throws std::invalid_argument if a raw value is negative, non-finite, or inconsistent.
 */
void
ValidateRawAccumulator(const StationPhyMetricAccumulator& accumulator)
{
    if (!std::isfinite(accumulator.nominalRateBpsNs) || accumulator.nominalRateBpsNs < 0.0L)
    {
        throw std::invalid_argument("station nominal-rate product must be finite and non-negative");
    }
    if (!std::isfinite(accumulator.psduBits) || accumulator.psduBits < 0.0L)
    {
        throw std::invalid_argument("station PSDU bits must be finite and non-negative");
    }
    if (accumulator.ppduAirtimeNs < 0)
    {
        throw std::invalid_argument("station PPDU airtime must be non-negative");
    }
    if (accumulator.contentionNs < 0)
    {
        throw std::invalid_argument("station contention time must be non-negative");
    }
    if (accumulator.ppduAirtimeNs == 0 &&
        (accumulator.nominalRateBpsNs != 0.0L || accumulator.psduBits != 0.0L))
    {
        throw std::invalid_argument("station rate or PSDU bits require positive PPDU airtime");
    }
}

} // namespace

void
StationPhyMetricAccumulator::Merge(const StationPhyMetricAccumulator& other)
{
    nominalRateBpsNs += other.nominalRateBpsNs;
    psduBits += other.psduBits;
    ppduAirtimeNs += other.ppduAirtimeNs;
    contentionNs += other.contentionNs;
}

StationPhyMetricOutput
DeriveStationPhyMetrics(const StationPhyMetricAccumulator& accumulator, int64_t windowDurationNs)
{
    ValidateRawAccumulator(accumulator);
    if (windowDurationNs < 0)
    {
        throw std::invalid_argument("station metric duration must be non-negative");
    }

    StationPhyMetricOutput output;
    if (accumulator.ppduAirtimeNs > 0)
    {
        const long double theoreticalMbps =
            accumulator.nominalRateBpsNs / accumulator.ppduAirtimeNs / 1'000'000.0L;
        const long double practicalMbps =
            accumulator.psduBits * 1000.0L / accumulator.ppduAirtimeNs;
        if (!std::isfinite(theoreticalMbps) || !std::isfinite(practicalMbps))
        {
            throw std::invalid_argument("derived station PHY rate must be finite");
        }
        const double theoreticalMbpsOutput = static_cast<double>(theoreticalMbps);
        const double practicalMbpsOutput = static_cast<double>(practicalMbps);
        if (!std::isfinite(theoreticalMbpsOutput) || !std::isfinite(practicalMbpsOutput))
        {
            throw std::invalid_argument("published station PHY rate must be finite");
        }
        const long double comparisonScale =
            std::max({1.0L, std::abs(theoreticalMbps), std::abs(practicalMbps)});
        if (practicalMbps - theoreticalMbps > METRIC_TOLERANCE * comparisonScale)
        {
            throw std::invalid_argument("practical station PHY rate exceeds theoretical rate");
        }

        output.averageTheoreticalPhyRateMbps = theoreticalMbpsOutput;
        output.averagePracticalPhyRateMbps = practicalMbpsOutput;
        if (theoreticalMbps > 0.0L)
        {
            output.channelEfficiency =
                ValidateUnitInterval(practicalMbps / theoreticalMbps, "channel efficiency");
        }
    }

    if (windowDurationNs > 0)
    {
        output.contentionFraction = ValidateUnitInterval(
            static_cast<long double>(accumulator.contentionNs) / windowDurationNs,
            "contention fraction");
    }
    else if (accumulator.contentionNs != 0)
    {
        throw std::invalid_argument("station contention time requires positive interval duration");
    }
    return output;
}

std::optional<StationPpduMetricContribution>
ExtractStationPpduMetricContribution(Mac48Address transmitterAddress,
                                     WifiPhyBand band,
                                     const WifiConstPsduMap& psduMap,
                                     const WifiTxVector& txVector)
{
    WifiConstPsduMap presentPsdus;
    long double qualifyingBytes = 0.0L;
    long double nominalRateBpsTimesBytes = 0.0L;
    for (const auto& [staId, psdu] : psduMap)
    {
        if (!psdu)
        {
            continue;
        }
        presentPsdus.emplace(staId, psdu);
        const uint64_t psduBytes = GetQualifyingPsduBytes(*psdu, transmitterAddress);
        if (psduBytes == 0)
        {
            continue;
        }
        const long double nominalRateBps = txVector.GetMode(staId).GetDataRate(txVector, staId);
        qualifyingBytes += psduBytes;
        nominalRateBpsTimesBytes += nominalRateBps * psduBytes;
    }
    if (qualifyingBytes == 0.0L)
    {
        return std::nullopt;
    }

    const int64_t ppduAirtimeNs =
        WifiPhy::CalculateTxDuration(presentPsdus, txVector, band).GetNanoSeconds();
    if (ppduAirtimeNs <= 0)
    {
        return std::nullopt;
    }
    return StationPpduMetricContribution{
        nominalRateBpsTimesBytes / qualifyingBytes,
        qualifyingBytes * 8.0L,
        ppduAirtimeNs,
    };
}

std::vector<std::pair<std::size_t, int64_t>>
SplitStationMetricInterval(int64_t intervalStartNs,
                           int64_t intervalEndNs,
                           int64_t measurementStartNs,
                           int64_t measurementEndNs,
                           int64_t windowDurationNs)
{
    std::vector<std::pair<std::size_t, int64_t>> splits;
    const int64_t clippedStartNs = std::max(intervalStartNs, measurementStartNs);
    const int64_t clippedEndNs = std::min(intervalEndNs, measurementEndNs);
    if (clippedStartNs >= clippedEndNs)
    {
        return splits;
    }

    int64_t positionNs = clippedStartNs;
    while (positionNs < clippedEndNs)
    {
        const std::size_t windowIndex =
            static_cast<std::size_t>((positionNs - measurementStartNs) / windowDurationNs);
        const int64_t windowEndNs =
            measurementStartNs + static_cast<int64_t>(windowIndex + 1) * windowDurationNs;
        const int64_t overlapEndNs = std::min(clippedEndNs, windowEndNs);
        splits.emplace_back(windowIndex, overlapEndNs - positionNs);
        positionNs = overlapEndNs;
    }
    return splits;
}

StationPhyMetricRecorder::StationPhyMetricRecorder(int64_t measurementStartNs,
                                                   int64_t measurementEndNs,
                                                   int64_t windowDurationNs)
    : m_measurementStartNs(measurementStartNs),
      m_measurementEndNs(measurementEndNs),
      m_windowDurationNs(windowDurationNs)
{
    if (measurementEndNs <= measurementStartNs)
    {
        throw std::invalid_argument("station metric measurement epoch must be non-empty");
    }
    if (windowDurationNs <= 0)
    {
        throw std::invalid_argument("station metric window duration must be positive");
    }
    if ((measurementEndNs - measurementStartNs) % windowDurationNs != 0)
    {
        throw std::invalid_argument("station metric epoch must contain complete windows");
    }
}

void
StationPhyMetricRecorder::RegisterStation(uint32_t stationId, Mac48Address transmitterAddress)
{
    if (transmitterAddress.IsGroup())
    {
        throw std::invalid_argument("station metric transmitter address must be unicast");
    }
    if (m_stations.contains(stationId))
    {
        throw std::invalid_argument("station metric identifier is already registered");
    }
    for (const auto& [registeredId, station] : m_stations)
    {
        static_cast<void>(registeredId);
        if (station.transmitterAddress == transmitterAddress)
        {
            throw std::invalid_argument("station metric transmitter address is already registered");
        }
    }

    const auto windowCount =
        static_cast<std::size_t>((m_measurementEndNs - m_measurementStartNs) / m_windowDurationNs);
    m_stations.emplace(stationId,
                       RegisteredStation{transmitterAddress,
                                         std::vector<StationPhyMetricAccumulator>(windowCount)});
}

void
StationPhyMetricRecorder::RecordPpduAttempt(uint32_t stationId,
                                            int64_t ppduStartNs,
                                            WifiPhyBand band,
                                            const WifiConstPsduMap& psduMap,
                                            const WifiTxVector& txVector)
{
    const auto station = m_stations.find(stationId);
    if (station == m_stations.end())
    {
        return;
    }
    const auto contribution =
        ExtractStationPpduMetricContribution(station->second.transmitterAddress,
                                             band,
                                             psduMap,
                                             txVector);
    if (!contribution)
    {
        return;
    }
    if (ppduStartNs > std::numeric_limits<int64_t>::max() - contribution->ppduAirtimeNs)
    {
        throw std::invalid_argument("station PPDU interval exceeds nanosecond range");
    }

    const int64_t ppduEndNs = ppduStartNs + contribution->ppduAirtimeNs;
    for (const auto& [windowIndex, overlapNs] : SplitStationMetricInterval(ppduStartNs,
                                                                           ppduEndNs,
                                                                           m_measurementStartNs,
                                                                           m_measurementEndNs,
                                                                           m_windowDurationNs))
    {
        auto& window = station->second.windows.at(windowIndex);
        const long double fraction =
            static_cast<long double>(overlapNs) / contribution->ppduAirtimeNs;
        window.nominalRateBpsNs += contribution->nominalRateBps * overlapNs;
        window.psduBits += contribution->psduBits * fraction;
        window.ppduAirtimeNs += overlapNs;
    }
}

void
StationPhyMetricRecorder::IngestContentionIntervals(
    uint32_t stationId,
    const std::vector<AccessWaitIntervalNs>& intervals)
{
    const auto station = m_stations.find(stationId);
    if (station == m_stations.end())
    {
        return;
    }

    std::vector<AccessWaitIntervalNs> unionIntervals = intervals;
    for (const auto& interval : unionIntervals)
    {
        if (interval.endNs < interval.startNs)
        {
            throw std::invalid_argument("station contention interval is reversed");
        }
    }
    std::sort(unionIntervals.begin(),
              unionIntervals.end(),
              [](const auto& left, const auto& right) {
                  return std::pair{left.startNs, left.endNs} <
                         std::pair{right.startNs, right.endNs};
              });
    std::vector<AccessWaitIntervalNs> normalized;
    for (const auto& interval : unionIntervals)
    {
        if (interval.startNs == interval.endNs)
        {
            continue;
        }
        if (normalized.empty() || interval.startNs > normalized.back().endNs)
        {
            normalized.push_back(interval);
        }
        else
        {
            normalized.back().endNs = std::max(normalized.back().endNs, interval.endNs);
        }
    }

    for (const auto& interval : normalized)
    {
        for (const auto& [windowIndex, overlapNs] : SplitStationMetricInterval(interval.startNs,
                                                                               interval.endNs,
                                                                               m_measurementStartNs,
                                                                               m_measurementEndNs,
                                                                               m_windowDurationNs))
        {
            station->second.windows.at(windowIndex).contentionNs += overlapNs;
        }
    }
}

const std::vector<StationPhyMetricAccumulator>&
StationPhyMetricRecorder::GetWindowAccumulators(uint32_t stationId) const
{
    return m_stations.at(stationId).windows;
}

StationPhyMetricAccumulator
StationPhyMetricRecorder::BuildOverallAccumulator(uint32_t stationId) const
{
    StationPhyMetricAccumulator overall;
    for (const auto& window : GetWindowAccumulators(stationId))
    {
        overall.Merge(window);
    }
    return overall;
}

} // namespace ns3
