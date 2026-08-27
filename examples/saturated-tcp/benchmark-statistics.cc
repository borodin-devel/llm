#include "benchmark-statistics.h"

#include "ns3/node.h"
#include "ns3/simulator.h"
#include "ns3/sta-wifi-mac.h"
#include "ns3/wifi-net-device.h"
#include "ns3/wifi-phy.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

namespace ns3
{

namespace
{

constexpr int64_t MEASUREMENT_DURATION_NS = 1'000'000'000; ///< Fixed benchmark duration.

bool
RawFloatingEqual(long double left, long double right)
{
    if (left == right)
    {
        return true;
    }
    if (!std::isfinite(left) || !std::isfinite(right))
    {
        return false;
    }
    constexpr long double ulpFactor = 128.0L;
    const long double scale = std::max({1.0L, std::abs(left), std::abs(right)});
    return std::abs(left - right) <=
           ulpFactor * std::numeric_limits<long double>::epsilon() * scale;
}

bool
RawProfilesEqual(const DataTxProfileMap& left, const DataTxProfileMap& right)
{
    if (left.size() != right.size())
    {
        return false;
    }
    auto rightIterator = right.begin();
    for (const auto& [key, value] : left)
    {
        if (rightIterator == right.end() || key != rightIterator->first)
        {
            return false;
        }
        const auto& other = rightIterator->second;
        if (!RawFloatingEqual(value.transmittedPsduBytes, other.transmittedPsduBytes) ||
            value.ppduAttemptCount != other.ppduAttemptCount ||
            value.ppduAirtimeNs != other.ppduAirtimeNs ||
            !RawFloatingEqual(value.nominalRateBps, other.nominalRateBps))
        {
            return false;
        }
        ++rightIterator;
    }
    return true;
}

DataTxProfileMap
MergeProfiles(const std::vector<DataTxProfileMap>& windows)
{
    DataTxProfileMap merged;
    for (const auto& window : windows)
    {
        for (const auto& [key, value] : window)
        {
            merged[key].Merge(value);
        }
    }
    return merged;
}

bool
HasDataActivity(const DataTxProfileMap& profiles)
{
    return std::ranges::any_of(profiles, [](const auto& item) {
        const auto& value = item.second;
        return value.transmittedPsduBytes > 0.0L || value.ppduAttemptCount > 0 ||
               value.ppduAirtimeNs > 0;
    });
}

StationStatisticsOutput
MakeStationOutput(const ExperimentEntityIdentity& identity, const PhyCategoryOutput& phy)
{
    StationStatisticsOutput output{identity.accessPointId,
                                   identity.stationIndex.value(),
                                   identity.nodeId,
                                   identity.nodeLabel,
                                   identity.ipv4,
                                   {}};
    output.statistics.phyStats = phy;
    return output;
}

AccessPointStatisticsOutput
MakeAccessPointOutput(const ExperimentEntityIdentity& identity, const PhyCategoryOutput& phy)
{
    AccessPointStatisticsOutput output{identity.accessPointId,
                                       identity.nodeId,
                                       identity.nodeLabel,
                                       identity.ipv4,
                                       {}};
    output.statistics.phyStats = phy;
    return output;
}

} // namespace

PhyCategoryOutput
DeriveStationDataTxMetrics(const DataTxProfileMap& profiles, int64_t intervalDurationNs)
{
    if (intervalDurationNs <= 0)
    {
        throw std::invalid_argument("station data TX interval duration must be positive");
    }

    PhyCategoryOutput output;
    long double totalBytes = 0.0L;
    int64_t totalAirtimeNs = 0;
    std::optional<DataTxProfileKey> dominantKey;
    long double dominantBytes = 0.0L;
    long double dominantRateBps = 0.0L;
    for (const auto& [key, raw] : profiles)
    {
        if ((key.channelWidthMhz != 20 && key.channelWidthMhz != 40 && key.channelWidthMhz != 80) ||
            key.nss == 0 || key.mcs > 11 || !std::isfinite(raw.transmittedPsduBytes) ||
            raw.transmittedPsduBytes < 0.0L || raw.ppduAirtimeNs < 0 ||
            !std::isfinite(raw.nominalRateBps) || raw.nominalRateBps < 0.0L)
        {
            throw std::invalid_argument("station data TX profile contains an invalid raw value");
        }
        if (raw.ppduAirtimeNs > std::numeric_limits<int64_t>::max() - totalAirtimeNs)
        {
            throw std::overflow_error("station data TX profile airtime sum overflowed");
        }
        const double bytes = static_cast<double>(raw.transmittedPsduBytes);
        const double airtimeUs = static_cast<double>(raw.ppduAirtimeNs) / 1000.0;
        if (!std::isfinite(bytes) || !std::isfinite(airtimeUs))
        {
            throw std::invalid_argument("station data TX profile exceeds public numeric range");
        }
        output.dataTxProfile.push_back(
            {key.channelWidthMhz, key.nss, key.mcs, bytes, raw.ppduAttemptCount, airtimeUs});
        totalBytes += raw.transmittedPsduBytes;
        totalAirtimeNs += raw.ppduAirtimeNs;
        if (!dominantKey || raw.transmittedPsduBytes > dominantBytes ||
            (raw.transmittedPsduBytes == dominantBytes && raw.nominalRateBps > dominantRateBps))
        {
            dominantKey = key;
            dominantBytes = raw.transmittedPsduBytes;
            dominantRateBps = raw.nominalRateBps;
        }
    }

    if (profiles.empty())
    {
        output.dataTxRateOverIntervalMbps = 0.0;
        return output;
    }
    if (!std::isfinite(totalBytes) || totalBytes <= 0.0L || totalAirtimeNs <= 0 || !dominantKey ||
        dominantRateBps <= 0.0L)
    {
        throw std::invalid_argument("populated station data TX profile has invalid totals");
    }

    const long double dominantRateMbps = dominantRateBps / 1'000'000.0L;
    const long double dominantShare = dominantBytes / totalBytes;
    const long double effectiveMbps = totalBytes * 8000.0L / totalAirtimeNs;
    const long double intervalMbps = totalBytes * 8000.0L / intervalDurationNs;
    long double gap = 1.0L - static_cast<long double>(totalAirtimeNs) / intervalDurationNs;
    constexpr long double boundaryTolerance = 1e-12L;
    if (gap < -boundaryTolerance || gap > 1.0L + boundaryTolerance)
    {
        throw std::invalid_argument("station data TX opportunity gap is outside [0, 1]");
    }
    gap = std::clamp(gap, 0.0L, 1.0L);
    for (const auto value : {dominantRateMbps, dominantShare, effectiveMbps, intervalMbps, gap})
    {
        if (!std::isfinite(value) || value < 0.0L)
        {
            throw std::invalid_argument("station derived data TX metric is invalid");
        }
    }
    output.dominantDataPhyRateMbps = static_cast<double>(dominantRateMbps);
    output.dominantDataProfileShare = static_cast<double>(dominantShare);
    output.effectivePhyRateMbps = static_cast<double>(effectiveMbps);
    output.dataTxRateOverIntervalMbps = static_cast<double>(intervalMbps);
    output.dataTxOpportunityGapFraction = static_cast<double>(gap);
    return output;
}

PhyCategoryOutput
DeriveBssDataTxMetrics(const std::vector<PhyCategoryOutput>& stations)
{
    PhyCategoryOutput output;
    long double dominantSum = 0.0L;
    long double effectiveSum = 0.0L;
    long double intervalSum = 0.0L;
    std::size_t dominantCount = 0;
    std::size_t effectiveCount = 0;
    for (const auto& station : stations)
    {
        if (station.meanDominantDataPhyRateMbps || station.meanEffectivePhyRateMbps ||
            station.aggregateDataTxRateOverIntervalMbps || !station.dataTxRateOverIntervalMbps)
        {
            throw std::invalid_argument("BSS derivation requires station-role PHY values");
        }
        const bool active = !station.dataTxProfile.empty();
        const bool completeActive =
            station.dominantDataPhyRateMbps && station.dominantDataProfileShare &&
            station.effectivePhyRateMbps && station.dataTxOpportunityGapFraction;
        const bool completeIdle =
            !station.dominantDataPhyRateMbps && !station.dominantDataProfileShare &&
            !station.effectivePhyRateMbps && !station.dataTxOpportunityGapFraction &&
            *station.dataTxRateOverIntervalMbps == 0.0;
        if ((active && !completeActive) || (!active && !completeIdle))
        {
            throw std::invalid_argument("BSS derivation requires a complete station role");
        }
        const auto IsFiniteNonnegative = [](double value) {
            return std::isfinite(value) && value >= 0.0;
        };
        if (!IsFiniteNonnegative(*station.dataTxRateOverIntervalMbps) ||
            (station.dominantDataPhyRateMbps &&
             !IsFiniteNonnegative(*station.dominantDataPhyRateMbps)) ||
            (station.effectivePhyRateMbps && !IsFiniteNonnegative(*station.effectivePhyRateMbps)))
        {
            throw std::invalid_argument("BSS derivation requires finite non-negative rates");
        }
        if (active &&
            (!std::isfinite(*station.dominantDataProfileShare) ||
             *station.dominantDataProfileShare <= 0.0 || *station.dominantDataProfileShare > 1.0 ||
             !std::isfinite(*station.dataTxOpportunityGapFraction) ||
             *station.dataTxOpportunityGapFraction < 0.0 ||
             *station.dataTxOpportunityGapFraction > 1.0))
        {
            throw std::invalid_argument("BSS derivation requires valid station fractions");
        }
        if (station.dominantDataPhyRateMbps)
        {
            dominantSum += *station.dominantDataPhyRateMbps;
            ++dominantCount;
        }
        if (station.effectivePhyRateMbps)
        {
            effectiveSum += *station.effectivePhyRateMbps;
            ++effectiveCount;
        }
        intervalSum += *station.dataTxRateOverIntervalMbps;
    }
    if (dominantCount > 0)
    {
        output.meanDominantDataPhyRateMbps = static_cast<double>(dominantSum / dominantCount);
    }
    if (effectiveCount > 0)
    {
        output.meanEffectivePhyRateMbps = static_cast<double>(effectiveSum / effectiveCount);
    }
    output.aggregateDataTxRateOverIntervalMbps = static_cast<double>(intervalSum);
    return output;
}

SaturatedTcpStatistics::SaturatedTcpStatistics(uint32_t windowMs)
    : m_windowMs(windowMs),
      m_windowNs(static_cast<int64_t>(windowMs) * 1'000'000)
{
    if (windowMs == 0 || 1000 % windowMs != 0)
    {
        throw std::invalid_argument(
            "saturated TCP statistics window must be positive and divide one second");
    }
}

SaturatedTcpStatistics::~SaturatedTcpStatistics()
{
    DisconnectAllTraceConnections();
}

SaturatedTcpStatistics::StationTraceConnectionGuard::StationTraceConnectionGuard(
    StationTraceConnections& connections)
    : m_connections(connections)
{
}

SaturatedTcpStatistics::StationTraceConnectionGuard::~StationTraceConnectionGuard()
{
    if (m_armed)
    {
        SaturatedTcpStatistics::DisconnectTraceConnections(m_connections);
    }
}

void
SaturatedTcpStatistics::StationTraceConnectionGuard::Disarm() noexcept
{
    m_armed = false;
}

void
SaturatedTcpStatistics::RegisterAccessPoint(uint32_t accessPointId,
                                            uint32_t nodeId,
                                            std::string nodeLabel,
                                            std::string ipv4)
{
    if (m_started)
    {
        throw std::logic_error("cannot register an access point after statistics start");
    }
    m_registry.RegisterAccessPoint(accessPointId, nodeId, std::move(nodeLabel), std::move(ipv4));
}

void
SaturatedTcpStatistics::RegisterStation(uint32_t accessPointId,
                                        uint32_t stationIndex,
                                        uint32_t nodeId,
                                        std::string nodeLabel,
                                        std::string ipv4)
{
    if (m_started)
    {
        throw std::logic_error("cannot register a station after statistics start");
    }
    m_registry.RegisterStation(accessPointId,
                               stationIndex,
                               nodeId,
                               std::move(nodeLabel),
                               std::move(ipv4));
}

void
SaturatedTcpStatistics::ConnectStation(Ptr<WifiNetDevice> device)
{
    if (m_started)
    {
        throw std::logic_error("cannot connect a station after statistics start");
    }
    if (!device || !device->GetNode())
    {
        throw std::invalid_argument("station statistics require a device attached to a node");
    }
    const uint32_t nodeId = device->GetNode()->GetId();
    const auto* identity = m_registry.FindByNodeId(nodeId);
    if (!identity || identity->kind != ExperimentEntityKind::STATION)
    {
        throw std::invalid_argument("station statistics cannot connect an AP or unknown device");
    }
    if (m_stationDevices.contains(nodeId))
    {
        throw std::invalid_argument("station statistics device is already connected");
    }
    if (!DynamicCast<StaWifiMac>(device->GetMac()))
    {
        throw std::invalid_argument("station statistics require ordinary StaWifiMac");
    }
    if (device->GetNPhys() == 0)
    {
        throw std::invalid_argument("station statistics require at least one station PHY");
    }

    StationTraceConnections connections;
    StationTraceConnectionGuard connectionGuard(connections);
    for (const auto& phy : device->GetPhys())
    {
        if (!phy)
        {
            throw std::invalid_argument("station statistics cannot connect a null station PHY");
        }
        connections.phyTraces.push_back(
            {phy,
             MakeCallback(&SaturatedTcpStatistics::NotifyPhyTxPsduBegin, this)
                 .Bind(nodeId, phy->GetPhyBand()),
             false});
        auto& connection = connections.phyTraces.back();
        if (!phy->TraceConnectWithoutContext("PhyTxPsduBegin", connection.callback))
        {
            throw std::invalid_argument("station PhyTxPsduBegin trace could not be connected");
        }
        connection.connected = true;
    }
    if (m_subscriptionOwnershipHook)
    {
        m_subscriptionOwnershipHook();
    }
    m_stationDevices.emplace(nodeId, device);
    try
    {
        m_traceConnections.emplace(nodeId, connections);
    }
    catch (...)
    {
        m_stationDevices.erase(nodeId);
        throw;
    }
    connectionGuard.Disarm();
}

void
SaturatedTcpStatistics::DisconnectTraceConnections(StationTraceConnections& connections) noexcept
{
    for (auto& connection : connections.phyTraces)
    {
        if (connection.source && connection.connected)
        {
            connection.source->TraceDisconnectWithoutContext("PhyTxPsduBegin", connection.callback);
        }
    }
    connections.phyTraces.clear();
}

void
SaturatedTcpStatistics::DisconnectAllTraceConnections() noexcept
{
    for (auto& [nodeId, connections] : m_traceConnections)
    {
        static_cast<void>(nodeId);
        DisconnectTraceConnections(connections);
    }
    m_traceConnections.clear();
}

void
SaturatedTcpStatistics::Start(int64_t experimentStartNs)
{
    if (m_started)
    {
        throw std::logic_error("saturated TCP statistics have already started");
    }
    if (m_registry.GetStations().size() != m_stationDevices.size())
    {
        throw std::invalid_argument("every registered station must be connected before start");
    }
    if (experimentStartNs > std::numeric_limits<int64_t>::max() - MEASUREMENT_DURATION_NS)
    {
        throw std::overflow_error("saturated TCP experiment endpoint exceeds nanosecond range");
    }
    m_experimentStartNs = experimentStartNs;
    m_experimentEndNs = experimentStartNs + MEASUREMENT_DURATION_NS;
    m_dataTxRecorder = std::make_unique<StationDataTxMetricRecorder>(m_experimentStartNs,
                                                                     m_experimentEndNs,
                                                                     m_windowNs);
    for (const auto& station : m_registry.GetStations())
    {
        const auto device = m_stationDevices.at(station.nodeId);
        m_dataTxRecorder->RegisterStation(station.nodeId, device->GetMac()->GetAddress());
    }
    m_started = true;
}

void
SaturatedTcpStatistics::Finalize(int64_t experimentEndNs)
{
    if (m_finalized)
    {
        return;
    }
    if (!m_started)
    {
        throw std::logic_error("cannot finalize saturated TCP statistics before start");
    }
    if (experimentEndNs != m_experimentEndNs)
    {
        throw std::invalid_argument("saturated TCP statistics must finalize at exactly one second");
    }
    m_finalized = true;
}

UnifiedExperimentSummary
SaturatedTcpStatistics::BuildSummary() const
{
    if (!m_finalized || !m_dataTxRecorder)
    {
        throw std::logic_error("cannot build saturated TCP summary before finalization");
    }
    std::map<uint32_t, std::vector<DataTxProfileMap>> rawWindows;
    std::map<uint32_t, DataTxProfileMap> rawOverall;
    for (const auto& station : m_registry.GetStations())
    {
        rawWindows.emplace(station.nodeId, m_dataTxRecorder->GetWindowProfiles(station.nodeId));
        rawOverall.emplace(station.nodeId, m_dataTxRecorder->GetOverallProfiles(station.nodeId));
    }
    return BuildSummaryFromRaw(rawWindows, rawOverall);
}

UnifiedExperimentSummary
SaturatedTcpStatistics::BuildSummaryFromRaw(
    const std::map<uint32_t, std::vector<DataTxProfileMap>>& rawWindows) const
{
    std::map<uint32_t, DataTxProfileMap> rawOverall;
    for (const auto& [nodeId, windows] : rawWindows)
    {
        rawOverall.emplace(nodeId, MergeProfiles(windows));
    }
    return BuildSummaryFromRaw(rawWindows, rawOverall);
}

UnifiedExperimentSummary
SaturatedTcpStatistics::BuildSummaryFromRaw(
    const std::map<uint32_t, std::vector<DataTxProfileMap>>& rawWindows,
    const std::map<uint32_t, DataTxProfileMap>& rawOverall) const
{
    const std::size_t windowCount = static_cast<std::size_t>(1000 / m_windowMs);
    for (const auto& station : m_registry.GetStations())
    {
        const auto raw = rawWindows.find(station.nodeId);
        if (raw == rawWindows.end() || raw->second.size() != windowCount)
        {
            throw std::invalid_argument("station raw windows do not cover the one-second epoch");
        }
        if (!rawOverall.contains(station.nodeId))
        {
            throw std::invalid_argument("station independent overall raw state is missing");
        }
    }

    UnifiedExperimentSummary summary;
    summary.statisticsWindowMs = m_windowMs;
    summary.accessPointInventory = m_registry.GetAccessPoints();
    summary.stationInventory = m_registry.GetStations();
    std::set<uint32_t> accessPointIds;
    for (const auto& accessPoint : summary.accessPointInventory)
    {
        accessPointIds.insert(accessPoint.accessPointId);
    }
    for (const auto& station : summary.stationInventory)
    {
        if (!accessPointIds.contains(station.accessPointId))
        {
            summary.validation.entityInventoryReferencesValid = false;
        }
    }

    for (std::size_t windowIndex = 0; windowIndex < windowCount; ++windowIndex)
    {
        ExperimentWindowOutput window{windowIndex,
                                      static_cast<double>(windowIndex * m_windowMs),
                                      static_cast<double>(m_windowMs),
                                      {},
                                      {}};
        std::map<uint32_t, std::vector<PhyCategoryOutput>> metricsByAccessPoint;
        std::set<uint32_t> activeAccessPoints;
        for (const auto& station : summary.stationInventory)
        {
            const auto& profiles = rawWindows.at(station.nodeId).at(windowIndex);
            const auto metrics = DeriveStationDataTxMetrics(profiles, m_windowNs);
            metricsByAccessPoint[station.accessPointId].push_back(metrics);
            if (HasDataActivity(profiles))
            {
                window.stations.push_back(MakeStationOutput(station, metrics));
                activeAccessPoints.insert(station.accessPointId);
            }
        }
        for (const auto& accessPoint : summary.accessPointInventory)
        {
            if (activeAccessPoints.contains(accessPoint.accessPointId))
            {
                window.accessPoints.push_back(MakeAccessPointOutput(
                    accessPoint,
                    DeriveBssDataTxMetrics(metricsByAccessPoint[accessPoint.accessPointId])));
            }
        }
        if (!window.stations.empty())
        {
            summary.windows.push_back(std::move(window));
        }
    }

    std::map<uint32_t, std::vector<PhyCategoryOutput>> overallByAccessPoint;
    for (const auto& station : summary.stationInventory)
    {
        const auto merged = MergeProfiles(rawWindows.at(station.nodeId));
        summary.validation.overallMatchesWindows =
            summary.validation.overallMatchesWindows &&
            RawProfilesEqual(rawOverall.at(station.nodeId), merged);
        const auto metrics =
            DeriveStationDataTxMetrics(rawOverall.at(station.nodeId), MEASUREMENT_DURATION_NS);
        overallByAccessPoint[station.accessPointId].push_back(metrics);
        summary.overall.stations.push_back(MakeStationOutput(station, metrics));
    }
    for (const auto& accessPoint : summary.accessPointInventory)
    {
        summary.overall.accessPoints.push_back(MakeAccessPointOutput(
            accessPoint,
            DeriveBssDataTxMetrics(overallByAccessPoint[accessPoint.accessPointId])));
    }
    return summary;
}

void
SaturatedTcpStatistics::NotifyPhyTxPsduBegin(uint32_t stationNodeId,
                                             WifiPhyBand band,
                                             WifiConstPsduMap psduMap,
                                             WifiTxVector txVector,
                                             double txPowerW)
{
    static_cast<void>(txPowerW);
    if (!m_started || m_finalized)
    {
        return;
    }
    m_dataTxRecorder->RecordPpduAttempt(stationNodeId,
                                        Simulator::Now().GetNanoSeconds(),
                                        band,
                                        psduMap,
                                        txVector);
}

} // namespace ns3
