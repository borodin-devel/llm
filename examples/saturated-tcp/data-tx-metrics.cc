#include "data-tx-metrics-internal.h"

#include "ns3/wifi-mpdu.h"
#include "ns3/wifi-phy.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace ns3
{

namespace
{

bool
IsQualifyingDataMpdu(const WifiMpdu& mpdu, Mac48Address transmitterAddress)
{
    const auto& header = mpdu.GetHeader();
    return header.HasData() && !header.GetAddr1().IsGroup() &&
           header.GetAddr2() == transmitterAddress;
}

uint64_t
GetQualifyingDataPsduBytes(const WifiPsdu& psdu, Mac48Address transmitterAddress)
{
    uint64_t qualifyingBytes = 0;
    std::size_t presentMpdus = 0;
    std::size_t qualifyingMpdus = 0;
    std::size_t mpduIndex = 0;
    for (const auto& mpdu : psdu)
    {
        if (mpdu)
        {
            ++presentMpdus;
            if (IsQualifyingDataMpdu(*mpdu, transmitterAddress))
            {
                ++qualifyingMpdus;
                qualifyingBytes +=
                    psdu.IsAggregate() ? psdu.GetAmpduSubframeSize(mpduIndex) : mpdu->GetSize();
            }
        }
        ++mpduIndex;
    }
    if (qualifyingMpdus == 0)
    {
        return 0;
    }
    return qualifyingMpdus == presentMpdus ? psdu.GetSize() : qualifyingBytes;
}

} // namespace

void
DataTxProfileAccumulator::Merge(const DataTxProfileAccumulator& other)
{
    if (nominalRateBps == 0.0L)
    {
        nominalRateBps = other.nominalRateBps;
    }
    else if (other.nominalRateBps != 0.0L && nominalRateBps != other.nominalRateBps)
    {
        throw std::invalid_argument("data TX profile nominal rates differ");
    }
    transmittedPsduBytes += other.transmittedPsduBytes;
    ppduAttemptCount += other.ppduAttemptCount;
    ppduAirtimeNs += other.ppduAirtimeNs;
}

std::optional<DataTxProfileContribution>
ExtractDataTxProfileContribution(Mac48Address transmitterAddress,
                                 WifiPhyBand band,
                                 const WifiConstPsduMap& psduMap,
                                 const WifiTxVector& txVector)
{
    WifiConstPsduMap presentPsdus;
    uint16_t staId = SU_STA_ID;
    uint64_t qualifyingBytes = 0;
    for (const auto& [candidateStaId, psdu] : psduMap)
    {
        if (!psdu)
        {
            continue;
        }
        if (!presentPsdus.empty())
        {
            throw std::invalid_argument("data TX profiles require one non-null SU PSDU");
        }
        presentPsdus.emplace(candidateStaId, psdu);
        staId = candidateStaId;
        qualifyingBytes = GetQualifyingDataPsduBytes(*psdu, transmitterAddress);
    }
    if (qualifyingBytes == 0)
    {
        return std::nullopt;
    }
    if (txVector.GetPreambleType() != WIFI_PREAMBLE_HE_SU || txVector.IsMu())
    {
        throw std::invalid_argument("data TX profiles require an HE SU transmission vector");
    }
    if (staId != SU_STA_ID)
    {
        throw std::invalid_argument("data TX profiles require the SU_STA_ID PSDU key");
    }

    const auto mode = txVector.GetMode(staId);
    if (mode.GetModulationClass() != WIFI_MOD_CLASS_HE)
    {
        throw std::invalid_argument("qualifying data TX vector must use HE modulation");
    }
    if (txVector.GetChannelWidth() != MHz_u{80})
    {
        throw std::invalid_argument("qualifying data TX vector must use 80 MHz");
    }
    if (txVector.GetGuardInterval() != NanoSeconds(3200))
    {
        throw std::invalid_argument("qualifying data TX vector must use a 3200 ns guard interval");
    }

    const auto nss = txVector.GetNss(staId);
    const auto mcs = mode.GetMcsValue();
    const auto rate = mode.GetDataRate(txVector, staId);
    const int64_t ppduAirtimeNs =
        WifiPhy::CalculateTxDuration(presentPsdus, txVector, band).GetNanoSeconds();
    if (ppduAirtimeNs <= 0)
    {
        throw std::invalid_argument("qualifying data PPDU duration must be positive");
    }
    return DataTxProfileContribution{{nss, mcs},
                                     static_cast<long double>(qualifyingBytes),
                                     ppduAirtimeNs,
                                     static_cast<long double>(rate)};
}

StationDataTxMetricRecorder::StationDataTxMetricRecorder(int64_t measurementStartNs,
                                                         int64_t measurementEndNs,
                                                         int64_t windowDurationNs)
    : m_measurementStartNs(measurementStartNs),
      m_measurementEndNs(measurementEndNs),
      m_windowDurationNs(windowDurationNs)
{
    if (measurementEndNs <= measurementStartNs)
    {
        throw std::invalid_argument("data TX measurement epoch must be non-empty");
    }
    if (windowDurationNs <= 0)
    {
        throw std::invalid_argument("data TX profile window duration must be positive");
    }
    if ((measurementEndNs - measurementStartNs) % windowDurationNs != 0)
    {
        throw std::invalid_argument("data TX measurement epoch must contain complete windows");
    }
}

void
StationDataTxMetricRecorder::RegisterStation(uint32_t stationId, Mac48Address transmitterAddress)
{
    if (transmitterAddress.IsGroup())
    {
        throw std::invalid_argument("data TX station address must be unicast");
    }
    if (m_stations.contains(stationId))
    {
        throw std::invalid_argument("data TX station identifier is already registered");
    }
    for (const auto& [registeredId, station] : m_stations)
    {
        static_cast<void>(registeredId);
        if (station.transmitterAddress == transmitterAddress)
        {
            throw std::invalid_argument("data TX station address is already registered");
        }
    }

    const auto windowCount =
        static_cast<std::size_t>((m_measurementEndNs - m_measurementStartNs) / m_windowDurationNs);
    m_stations.emplace(
        stationId,
        RegisteredStation{transmitterAddress, std::vector<DataTxProfileMap>(windowCount), {}});
}

void
StationDataTxMetricRecorder::RecordPpduAttempt(uint32_t stationId,
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
    const auto contribution = ExtractDataTxProfileContribution(station->second.transmitterAddress,
                                                               band,
                                                               psduMap,
                                                               txVector);
    if (!contribution)
    {
        return;
    }
    if (ppduStartNs > std::numeric_limits<int64_t>::max() - contribution->ppduAirtimeNs)
    {
        throw std::invalid_argument("data TX PPDU interval exceeds nanosecond range");
    }
    const int64_t ppduEndNs = ppduStartNs + contribution->ppduAirtimeNs;
    const int64_t clippedStartNs = std::max(ppduStartNs, m_measurementStartNs);
    const int64_t clippedEndNs = std::min(ppduEndNs, m_measurementEndNs);
    if (clippedStartNs >= clippedEndNs)
    {
        return;
    }

    const bool startsInEpoch =
        ppduStartNs >= m_measurementStartNs && ppduStartNs < m_measurementEndNs;
    const std::size_t startWindowIndex =
        startsInEpoch
            ? static_cast<std::size_t>((ppduStartNs - m_measurementStartNs) / m_windowDurationNs)
            : station->second.windows.size();
    int64_t positionNs = clippedStartNs;
    while (positionNs < clippedEndNs)
    {
        const std::size_t windowIndex =
            static_cast<std::size_t>((positionNs - m_measurementStartNs) / m_windowDurationNs);
        const int64_t windowEndNs =
            m_measurementStartNs + static_cast<int64_t>(windowIndex + 1) * m_windowDurationNs;
        const int64_t overlapEndNs = std::min(clippedEndNs, windowEndNs);
        const int64_t overlapNs = overlapEndNs - positionNs;
        const long double fraction =
            static_cast<long double>(overlapNs) / contribution->ppduAirtimeNs;
        station->second.windows.at(windowIndex)[contribution->key].Merge(
            {contribution->transmittedPsduBytes * fraction,
             windowIndex == startWindowIndex ? 1U : 0U,
             overlapNs,
             contribution->nominalRateBps});
        positionNs = overlapEndNs;
    }

    const int64_t overallOverlapNs = clippedEndNs - clippedStartNs;
    const long double overallFraction =
        static_cast<long double>(overallOverlapNs) / contribution->ppduAirtimeNs;
    station->second.overall[contribution->key].Merge(
        {contribution->transmittedPsduBytes * overallFraction,
         startsInEpoch ? 1U : 0U,
         overallOverlapNs,
         contribution->nominalRateBps});
}

const std::vector<DataTxProfileMap>&
StationDataTxMetricRecorder::GetWindowProfiles(uint32_t stationId) const
{
    return m_stations.at(stationId).windows;
}

const DataTxProfileMap&
StationDataTxMetricRecorder::GetOverallProfiles(uint32_t stationId) const
{
    return m_stations.at(stationId).overall;
}

} // namespace ns3
