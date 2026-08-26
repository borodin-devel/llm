#include "benchmark-statistics.h"

#include "access-tracking-sta-wifi-mac.h"
#include "access-wait-tracker.h"

#include "ns3/mac48-address.h"
#include "ns3/node.h"
#include "ns3/qos-txop.h"
#include "ns3/simulator.h"
#include "ns3/wifi-mac.h"
#include "ns3/wifi-net-device.h"
#include "ns3/wifi-phy.h"

#include <algorithm>
#include <array>
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

/** Determine whether one raw station window contains PPDU or contention activity. */
bool
HasStationActivity(const StationPhyMetricAccumulator& accumulator)
{
    return accumulator.nominalRateBpsNs != 0.0L || accumulator.psduBits != 0.0L ||
           accumulator.ppduAirtimeNs != 0 || accumulator.contentionNs != 0;
}

/** Copy one station metric output into the shared PHY DTO. */
void
SetPhyMetrics(PhyCategoryOutput& phy, const StationPhyMetricOutput& metrics)
{
    phy.averageTheoreticalPhyRateMbps = metrics.averageTheoreticalPhyRateMbps;
    phy.averagePracticalPhyRateMbps = metrics.averagePracticalPhyRateMbps;
    phy.channelEfficiency = metrics.channelEfficiency;
    phy.contentionFraction = metrics.contentionFraction;
}

/** Construct a shared station DTO from identity and derived metrics. */
StationStatisticsOutput
MakeStationOutput(const ExperimentEntityIdentity& identity, const StationPhyMetricOutput& metrics)
{
    StationStatisticsOutput output{identity.accessPointId,
                                   identity.stationIndex.value(),
                                   identity.nodeId,
                                   identity.nodeLabel,
                                   identity.ipv4,
                                   {}};
    SetPhyMetrics(output.statistics.phyStats, metrics);
    return output;
}

/** Derive station-arithmetic BSS fields. */
StationPhyMetricOutput
BuildAccessPointMetrics(const std::vector<StationPhyMetricOutput>& stations)
{
    long double theoreticalSum = 0.0L;
    long double practicalSum = 0.0L;
    long double contentionSum = 0.0L;
    std::size_t theoreticalCount = 0;
    std::size_t practicalCount = 0;
    std::size_t contentionCount = 0;
    for (const auto& station : stations)
    {
        if (station.averageTheoreticalPhyRateMbps)
        {
            theoreticalSum += *station.averageTheoreticalPhyRateMbps;
            ++theoreticalCount;
        }
        if (station.averagePracticalPhyRateMbps)
        {
            practicalSum += *station.averagePracticalPhyRateMbps;
            ++practicalCount;
        }
        if (station.contentionFraction)
        {
            contentionSum += *station.contentionFraction;
            ++contentionCount;
        }
    }

    StationPhyMetricOutput output;
    if (theoreticalCount > 0)
    {
        output.averageTheoreticalPhyRateMbps =
            static_cast<double>(theoreticalSum / theoreticalCount);
    }
    if (practicalCount > 0)
    {
        output.averagePracticalPhyRateMbps = static_cast<double>(practicalSum / practicalCount);
    }
    if (output.averageTheoreticalPhyRateMbps && output.averagePracticalPhyRateMbps &&
        *output.averageTheoreticalPhyRateMbps > 0.0)
    {
        output.channelEfficiency =
            *output.averagePracticalPhyRateMbps / *output.averageTheoreticalPhyRateMbps;
    }
    if (contentionCount > 0)
    {
        output.contentionFraction = static_cast<double>(contentionSum / contentionCount);
    }
    return output;
}

/** Construct a shared AP DTO from identity and station-derived metrics. */
AccessPointStatisticsOutput
MakeAccessPointOutput(const ExperimentEntityIdentity& identity,
                      const StationPhyMetricOutput& metrics)
{
    AccessPointStatisticsOutput output{identity.accessPointId,
                                       identity.nodeId,
                                       identity.nodeLabel,
                                       identity.ipv4,
                                       {}};
    SetPhyMetrics(output.statistics.phyStats, metrics);
    return output;
}

/** Compare exact raw station accumulator state. */
bool
RawMetricsEqual(const StationPhyMetricAccumulator& left, const StationPhyMetricAccumulator& right)
{
    return left.nominalRateBpsNs == right.nominalRateBpsNs && left.psduBits == right.psduBits &&
           left.ppduAirtimeNs == right.ppduAirtimeNs && left.contentionNs == right.contentionNs;
}

} // namespace

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

SaturatedTcpStatistics::~SaturatedTcpStatistics() = default;

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

    const auto mac = DynamicCast<AccessTrackingStaWifiMac>(device->GetMac());
    if (!mac)
    {
        throw std::invalid_argument("station statistics require AccessTrackingStaWifiMac");
    }
    if (device->GetNPhys() == 0)
    {
        throw std::invalid_argument("station statistics require at least one station PHY");
    }

    std::array<Ptr<QosTxop>, 4> txops;
    std::size_t txopIndex = 0;
    for (const auto ac : {AC_BE, AC_BK, AC_VI, AC_VO})
    {
        const auto txop = mac->GetQosTxop(ac);
        if (!txop)
        {
            throw std::invalid_argument("station statistics require every QoS TXOP");
        }
        txops.at(txopIndex++) = txop;
    }
    for (const auto& phy : device->GetPhys())
    {
        if (!phy)
        {
            throw std::invalid_argument("station statistics cannot connect a null station PHY");
        }
    }

    if (!mac->TraceConnectWithoutContext(
            "AccessRequested",
            MakeCallback(&SaturatedTcpStatistics::NotifyAccessRequested, this).Bind(nodeId)))
    {
        throw std::invalid_argument("station AccessRequested trace could not be connected");
    }
    for (const auto& txop : txops)
    {
        const uint8_t ac = txop->GetAccessCategory();
        if (!txop->TraceConnectWithoutContext(
                "TxopTrace",
                MakeCallback(&SaturatedTcpStatistics::NotifyTxopGranted, this).Bind(nodeId, ac)))
        {
            throw std::invalid_argument("station TXOP trace could not be connected");
        }
    }
    for (const auto& phy : device->GetPhys())
    {
        if (!phy->TraceConnectWithoutContext(
                "PhyTxPsduBegin",
                MakeCallback(&SaturatedTcpStatistics::NotifyPhyTxPsduBegin, this)
                    .Bind(nodeId, phy->GetPhyBand())))
        {
            throw std::invalid_argument("station PhyTxPsduBegin trace could not be connected");
        }
    }

    m_stationDevices.emplace(nodeId, device);
    m_stationMacs.emplace(nodeId, mac);
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
    m_phyRecorder = std::make_unique<StationPhyMetricRecorder>(m_experimentStartNs,
                                                               m_experimentEndNs,
                                                               m_windowNs);
    for (const auto& station : m_registry.GetStations())
    {
        const auto device = m_stationDevices.at(station.nodeId);
        m_phyRecorder->RegisterStation(station.nodeId, device->GetMac()->GetAddress());
        m_accessWaitTrackers.emplace(
            station.nodeId,
            std::make_unique<AccessWaitTracker>(m_experimentStartNs, m_experimentEndNs));
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

    for (const auto& station : m_registry.GetStations())
    {
        auto& tracker = *m_accessWaitTrackers.at(station.nodeId);
        tracker.Finalize(m_stationMacs.at(station.nodeId)->GetActiveTxopStartTimes());
        m_phyRecorder->IngestContentionIntervals(station.nodeId, tracker.GetUnionIntervals());
    }
    m_finalized = true;
}

UnifiedExperimentSummary
SaturatedTcpStatistics::BuildSummary() const
{
    if (!m_finalized || !m_phyRecorder)
    {
        throw std::logic_error("cannot build saturated TCP summary before finalization");
    }

    std::map<uint32_t, std::vector<StationPhyMetricAccumulator>> rawWindows;
    for (const auto& station : m_registry.GetStations())
    {
        rawWindows.emplace(station.nodeId, m_phyRecorder->GetWindowAccumulators(station.nodeId));
    }
    auto summary = BuildSummaryFromRaw(rawWindows);
    for (const auto& station : m_registry.GetStations())
    {
        StationPhyMetricAccumulator mergedWindows;
        for (const auto& window : rawWindows.at(station.nodeId))
        {
            mergedWindows.Merge(window);
        }
        summary.validation.overallMatchesWindows =
            summary.validation.overallMatchesWindows &&
            RawMetricsEqual(m_phyRecorder->BuildOverallAccumulator(station.nodeId), mergedWindows);
    }
    return summary;
}

UnifiedExperimentSummary
SaturatedTcpStatistics::BuildSummaryFromRaw(
    const std::map<uint32_t, std::vector<StationPhyMetricAccumulator>>& rawWindows) const
{
    const std::size_t windowCount = static_cast<std::size_t>(1000 / m_windowMs);
    for (const auto& station : m_registry.GetStations())
    {
        const auto raw = rawWindows.find(station.nodeId);
        if (raw == rawWindows.end() || raw->second.size() != windowCount)
        {
            throw std::invalid_argument("station raw windows do not cover the one-second epoch");
        }
    }

    UnifiedExperimentSummary summary;
    summary.statisticsWindowMs = m_windowMs;
    summary.accessPointInventory = m_registry.GetAccessPoints();
    summary.stationInventory = m_registry.GetStations();

    std::set<uint32_t> accessPointIds;
    for (const auto& accessPoint : m_registry.GetAccessPoints())
    {
        accessPointIds.insert(accessPoint.accessPointId);
    }
    for (const auto& station : m_registry.GetStations())
    {
        if (!accessPointIds.contains(station.accessPointId))
        {
            summary.validation.entityInventoryReferencesValid = false;
        }
    }

    for (std::size_t windowIndex = 0; windowIndex < windowCount; ++windowIndex)
    {
        ExperimentWindowOutput window;
        window.windowIndex = windowIndex;
        window.windowStartMs = static_cast<double>(windowIndex) * m_windowMs;
        window.windowDurationMs = m_windowMs;
        std::map<uint32_t, std::vector<StationPhyMetricOutput>> metricsByAccessPoint;
        std::set<uint32_t> activeAccessPoints;

        for (const auto& station : m_registry.GetStations())
        {
            const auto& raw = rawWindows.at(station.nodeId).at(windowIndex);
            const auto metrics = DeriveStationPhyMetrics(raw, m_windowNs);
            metricsByAccessPoint[station.accessPointId].push_back(metrics);
            if (HasStationActivity(raw))
            {
                window.stations.push_back(MakeStationOutput(station, metrics));
                activeAccessPoints.insert(station.accessPointId);
            }
        }
        for (const auto& accessPoint : m_registry.GetAccessPoints())
        {
            if (activeAccessPoints.contains(accessPoint.accessPointId))
            {
                window.accessPoints.push_back(MakeAccessPointOutput(
                    accessPoint,
                    BuildAccessPointMetrics(metricsByAccessPoint[accessPoint.accessPointId])));
            }
        }
        if (!window.stations.empty())
        {
            summary.windows.push_back(std::move(window));
        }
    }

    std::map<uint32_t, std::vector<StationPhyMetricOutput>> overallMetricsByAccessPoint;
    for (const auto& station : m_registry.GetStations())
    {
        StationPhyMetricAccumulator overall;
        for (const auto& window : rawWindows.at(station.nodeId))
        {
            overall.Merge(window);
        }
        StationPhyMetricAccumulator reproduced;
        for (const auto& window : rawWindows.at(station.nodeId))
        {
            reproduced.Merge(window);
        }
        summary.validation.overallMatchesWindows =
            summary.validation.overallMatchesWindows && RawMetricsEqual(overall, reproduced);

        const auto metrics = DeriveStationPhyMetrics(overall, MEASUREMENT_DURATION_NS);
        if (!metrics.averageTheoreticalPhyRateMbps || !metrics.averagePracticalPhyRateMbps)
        {
            throw std::invalid_argument(
                "saturated overall station PHY rates are undefined for node " +
                std::to_string(station.nodeId));
        }
        overallMetricsByAccessPoint[station.accessPointId].push_back(metrics);
        summary.overall.stations.push_back(MakeStationOutput(station, metrics));
    }
    for (const auto& accessPoint : m_registry.GetAccessPoints())
    {
        summary.overall.accessPoints.push_back(MakeAccessPointOutput(
            accessPoint,
            BuildAccessPointMetrics(overallMetricsByAccessPoint[accessPoint.accessPointId])));
    }
    return summary;
}

void
SaturatedTcpStatistics::NotifyAccessRequested(uint32_t stationNodeId, uint8_t ac, uint8_t linkId)
{
    if (!m_started || m_finalized)
    {
        return;
    }
    m_accessWaitTrackers.at(stationNodeId)
        ->NotifyRequest(ac, linkId, Simulator::Now().GetNanoSeconds());
}

void
SaturatedTcpStatistics::NotifyTxopGranted(uint32_t stationNodeId,
                                          uint8_t ac,
                                          Time start,
                                          Time duration,
                                          uint8_t linkId)
{
    static_cast<void>(duration);
    if (!m_started || m_finalized)
    {
        return;
    }
    m_accessWaitTrackers.at(stationNodeId)->NotifyGrant(ac, linkId, start.GetNanoSeconds());
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
    m_phyRecorder->RecordPpduAttempt(stationNodeId,
                                     Simulator::Now().GetNanoSeconds(),
                                     band,
                                     psduMap,
                                     txVector);
}

} // namespace ns3
