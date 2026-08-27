#include "../statistics/json/writer.h"
#include "benchmark-statistics.h"
#include "config.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ns3
{

namespace
{

constexpr double METRIC_TOLERANCE = 1e-9; ///< Relative benchmark validation tolerance.

/** Reject a false benchmark-summary invariant. */
void
Require(bool condition, const std::string& message)
{
    if (!condition)
    {
        throw std::invalid_argument("invalid saturated benchmark summary: " + message);
    }
}

/** Compare finite public values with the benchmark relative tolerance. */
bool
NearlyEqual(double left, double right)
{
    const double scale = std::max({1.0, std::abs(left), std::abs(right)});
    return std::abs(left - right) <= METRIC_TOLERANCE * scale;
}

/** Validate one optional non-negative finite value. */
void
ValidateOptionalRate(const std::optional<double>& value, const char* name)
{
    if (value)
    {
        Require(std::isfinite(*value) && *value >= 0.0,
                std::string(name) + " must be finite and non-negative");
    }
}

/** Validate the four benchmark PHY fields for one station or AP. */
void
ValidateMetricFields(const PhyCategoryOutput& phy, const char* entity)
{
    ValidateOptionalRate(phy.averageTheoreticalPhyRateMbps, "theoretical PHY rate");
    ValidateOptionalRate(phy.averagePracticalPhyRateMbps, "practical PHY rate");
    Require(phy.averageTheoreticalPhyRateMbps.has_value() ==
                phy.averagePracticalPhyRateMbps.has_value(),
            std::string(entity) + " theoretical and practical rate presence differs");
    if (phy.averageTheoreticalPhyRateMbps)
    {
        const double theoretical = *phy.averageTheoreticalPhyRateMbps;
        const double practical = *phy.averagePracticalPhyRateMbps;
        const double scale = std::max({1.0, theoretical, practical});
        Require(practical - theoretical <= METRIC_TOLERANCE * scale,
                std::string(entity) + " practical PHY rate exceeds theoretical rate");
        if (theoretical > 0.0)
        {
            Require(phy.channelEfficiency.has_value(),
                    std::string(entity) + " channel efficiency is undefined");
            Require(NearlyEqual(*phy.channelEfficiency, practical / theoretical),
                    std::string(entity) + " channel efficiency does not reproduce its rates");
        }
        else
        {
            Require(practical == 0.0 && !phy.channelEfficiency,
                    std::string(entity) + " zero theoretical rate has invalid efficiency");
        }
    }
    else
    {
        Require(!phy.channelEfficiency,
                std::string(entity) + " channel efficiency exists without PHY rates");
    }

    if (phy.channelEfficiency)
    {
        Require(std::isfinite(*phy.channelEfficiency) &&
                    *phy.channelEfficiency >= -METRIC_TOLERANCE &&
                    *phy.channelEfficiency <= 1.0 + METRIC_TOLERANCE,
                std::string(entity) + " channel efficiency is outside [0, 1]");
    }
    Require(phy.contentionFraction.has_value(),
            std::string(entity) + " contention fraction is undefined");
    Require(std::isfinite(*phy.contentionFraction) &&
                *phy.contentionFraction >= -METRIC_TOLERANCE &&
                *phy.contentionFraction <= 1.0 + METRIC_TOLERANCE,
            std::string(entity) + " contention fraction is outside [0, 1]");
}

/** Serialize entity statistics after removing the four benchmark fields. */
std::string
SerializeNonBenchmarkStatistics(EntityStatisticsOutput statistics)
{
    statistics.phyStats.averageTheoreticalPhyRateMbps.reset();
    statistics.phyStats.averagePracticalPhyRateMbps.reset();
    statistics.phyStats.channelEfficiency.reset();
    statistics.phyStats.contentionFraction.reset();
    std::ostringstream output;
    JsonWriter writer(output);
    writer.BeginObject();
    WriteEntityStatisticsJson(writer, statistics);
    writer.EndObject();
    writer.Finish();
    return output.str();
}

/** Determine whether a default sample distribution has any derived value present. */
bool
HasDerivedValue(const SampleDistributionOutput& distribution)
{
    return distribution.averageUs || distribution.standardDeviationUs || distribution.minimumUs ||
           distribution.maximumUs;
}

/** Determine whether unrelated top-level optional fields are present. */
bool
HasUnrelatedOptionalValue(const EntityStatisticsOutput& statistics)
{
    const auto hasGeneral = [](const GeneralDirectionOutput& direction) {
        return direction.averageTransmissionDurationUs ||
               direction.transmissionDurationStandardDeviationUs ||
               direction.minimumTransmissionDurationUs || direction.maximumTransmissionDurationUs ||
               direction.effectiveThroughputMbps ||
               HasDerivedValue(direction.applicationToPhyDelay);
    };
    const auto hasApp = [](const AppDirectionOutput& direction) {
        return direction.acceptedThroughputMbps || direction.receivedThroughputMbps ||
               HasDerivedValue(direction.receiveInterArrivalTime);
    };
    const auto hasMac = [](const MacDirectionOutput& direction) {
        return direction.estimatedTransmitThroughputMbps ||
               direction.estimatedReceiveThroughputMbps;
    };
    const auto hasPhy = [](const PhyDirectionOutput& direction) {
        return direction.averageDataRateMbps || direction.throughputMbps;
    };
    return hasGeneral(statistics.generalStats.uplink) ||
           hasGeneral(statistics.generalStats.downlink) || hasApp(statistics.appStats.uplink) ||
           hasApp(statistics.appStats.downlink) || hasMac(statistics.macStats.uplink) ||
           hasMac(statistics.macStats.downlink) || statistics.phyStats.channelUtilizationPercent ||
           hasPhy(statistics.phyStats.uplink) || hasPhy(statistics.phyStats.downlink);
}

/** Require every non-benchmark entity category and PHY field to remain default. */
void
ValidateDefaultCategories(const EntityStatisticsOutput& statistics, const char* entity)
{
    static const std::string defaults = SerializeNonBenchmarkStatistics({});
    Require(!HasUnrelatedOptionalValue(statistics) &&
                SerializeNonBenchmarkStatistics(statistics) == defaults,
            std::string(entity) + " contains non-default unrelated statistics");
}

/** Compare optional metric values exactly within public output tolerance. */
void
RequireOptionalEqual(const std::optional<double>& actual,
                     const std::optional<double>& expected,
                     const char* name)
{
    Require(actual.has_value() == expected.has_value(),
            std::string("AP ") + name + " presence does not match station arithmetic");
    if (actual)
    {
        Require(NearlyEqual(*actual, *expected),
                std::string("AP ") + name + " does not match station arithmetic");
    }
}

/** Derive expected AP fields from station DTOs and the complete BSS station count. */
StationPhyMetricOutput
BuildExpectedAccessPointMetrics(const std::vector<const StationStatisticsOutput*>& stations,
                                std::size_t stationCount)
{
    long double theoreticalSum = 0.0L;
    long double practicalSum = 0.0L;
    long double contentionSum = 0.0L;
    std::size_t theoreticalCount = 0;
    std::size_t practicalCount = 0;
    for (const auto* station : stations)
    {
        const auto& phy = station->statistics.phyStats;
        if (phy.averageTheoreticalPhyRateMbps)
        {
            theoreticalSum += *phy.averageTheoreticalPhyRateMbps;
            ++theoreticalCount;
        }
        if (phy.averagePracticalPhyRateMbps)
        {
            practicalSum += *phy.averagePracticalPhyRateMbps;
            ++practicalCount;
        }
        contentionSum += *phy.contentionFraction;
    }

    StationPhyMetricOutput expected;
    if (theoreticalCount > 0)
    {
        expected.averageTheoreticalPhyRateMbps =
            static_cast<double>(theoreticalSum / theoreticalCount);
    }
    if (practicalCount > 0)
    {
        expected.averagePracticalPhyRateMbps = static_cast<double>(practicalSum / practicalCount);
    }
    if (expected.averageTheoreticalPhyRateMbps && expected.averagePracticalPhyRateMbps &&
        *expected.averageTheoreticalPhyRateMbps > 0.0)
    {
        expected.channelEfficiency =
            *expected.averagePracticalPhyRateMbps / *expected.averageTheoreticalPhyRateMbps;
    }
    if (stationCount > 0)
    {
        expected.contentionFraction = static_cast<double>(contentionSum / stationCount);
    }
    return expected;
}

/** Validate an AP DTO against its station-derived arithmetic. */
void
ValidateAccessPointMetrics(const AccessPointStatisticsOutput& accessPoint,
                           const std::vector<const StationStatisticsOutput*>& stations,
                           std::size_t stationCount)
{
    ValidateDefaultCategories(accessPoint.statistics, "AP");
    ValidateMetricFields(accessPoint.statistics.phyStats, "AP");
    const auto expected = BuildExpectedAccessPointMetrics(stations, stationCount);
    const auto& actual = accessPoint.statistics.phyStats;
    RequireOptionalEqual(actual.averageTheoreticalPhyRateMbps,
                         expected.averageTheoreticalPhyRateMbps,
                         "theoretical PHY rate");
    RequireOptionalEqual(actual.averagePracticalPhyRateMbps,
                         expected.averagePracticalPhyRateMbps,
                         "practical PHY rate");
    RequireOptionalEqual(actual.channelEfficiency, expected.channelEfficiency, "efficiency");
    RequireOptionalEqual(actual.contentionFraction,
                         expected.contentionFraction,
                         "contention fraction");
}

/** Validate an access point output identity against inventory. */
void
ValidateAccessPointIdentity(const AccessPointStatisticsOutput& output,
                            const ExperimentEntityIdentity& identity)
{
    Require(output.accessPointId == identity.accessPointId && output.nodeId == identity.nodeId &&
                output.nodeLabel == identity.nodeLabel && output.ipv4 == identity.ipv4,
            "AP output identity does not match inventory");
}

/** Validate a station output identity against inventory. */
void
ValidateStationIdentity(const StationStatisticsOutput& output,
                        const ExperimentEntityIdentity& identity)
{
    Require(output.accessPointId == identity.accessPointId &&
                output.stationIndex == identity.stationIndex && output.nodeId == identity.nodeId &&
                output.nodeLabel == identity.nodeLabel && output.ipv4 == identity.ipv4,
            "station output identity does not match inventory");
}

/** Require all eight shared validation flags to be true. */
void
ValidateFlags(const ExperimentValidationOutput& validation)
{
    Require(validation.entityInventoryReferencesValid, "entity inventory references are invalid");
    Require(validation.appAgentTotalsConsistent, "application agent totals are inconsistent");
    Require(validation.appPeerTotalsConsistent, "application peer totals are inconsistent");
    Require(validation.macPeerTotalsConsistent, "MAC peer totals are inconsistent");
    Require(validation.phyPeerTotalsConsistent, "PHY peer totals are inconsistent");
    Require(validation.apStationSenderTotalsConsistent,
            "AP and station sender totals are inconsistent");
    Require(validation.overallMatchesWindows, "overall does not match merged windows");
    Require(validation.uniquePhyPayloadWithinTaggedPayload,
            "unique PHY payload exceeds tagged payload");
}

/** Validate inventory shape and return station identities by BSS/index. */
std::map<std::pair<uint32_t, uint32_t>, const ExperimentEntityIdentity*>
ValidateInventory(const UnifiedExperimentSummary& summary, const SaturatedTcpConfig& config)
{
    Require(summary.accessPointInventory.size() == 3,
            "benchmark inventory must contain exactly three access points");
    Require(summary.stationInventory.size() ==
                3 * static_cast<std::size_t>(config.benchmark.stationCountPerBss),
            "station inventory does not match benchmark.sta_count_per_bss");

    std::set<uint32_t> nodeIds;
    std::set<std::string> ipv4Addresses;
    for (uint32_t accessPointId = 0; accessPointId < 3; ++accessPointId)
    {
        const auto& identity = summary.accessPointInventory.at(accessPointId);
        Require(identity.kind == ExperimentEntityKind::ACCESS_POINT &&
                    identity.accessPointId == accessPointId && !identity.stationIndex,
                "access point inventory order or kind is invalid");
        Require(nodeIds.insert(identity.nodeId).second,
                "inventory contains a duplicate node identifier");
        Require(ipv4Addresses.insert(identity.ipv4).second,
                "inventory contains a duplicate IPv4 address");
    }

    std::map<std::pair<uint32_t, uint32_t>, const ExperimentEntityIdentity*> stations;
    std::size_t position = 0;
    for (uint32_t accessPointId = 0; accessPointId < 3; ++accessPointId)
    {
        for (uint32_t stationIndex = 0; stationIndex < config.benchmark.stationCountPerBss;
             ++stationIndex)
        {
            const auto& identity = summary.stationInventory.at(position++);
            Require(identity.kind == ExperimentEntityKind::STATION &&
                        identity.accessPointId == accessPointId &&
                        identity.stationIndex == stationIndex,
                    "station inventory order, parent, or index is invalid");
            Require(nodeIds.insert(identity.nodeId).second,
                    "inventory contains a duplicate node identifier");
            Require(ipv4Addresses.insert(identity.ipv4).second,
                    "inventory contains a duplicate IPv4 address");
            stations.emplace(std::pair{accessPointId, stationIndex}, &identity);
        }
    }
    return stations;
}

/** Validate one sparse benchmark window. */
void
ValidateWindow(const ExperimentWindowOutput& window,
               const UnifiedExperimentSummary& summary,
               const SaturatedTcpConfig& config,
               const std::map<std::pair<uint32_t, uint32_t>, const ExperimentEntityIdentity*>&
                   stationInventory)
{
    Require(window.windowIndex < 1000 / summary.statisticsWindowMs,
            "window index exceeds the one-second epoch");
    Require(NearlyEqual(window.windowStartMs,
                        static_cast<double>(window.windowIndex) * summary.statisticsWindowMs),
            "window start does not match its index");
    Require(NearlyEqual(window.windowDurationMs, summary.statisticsWindowMs),
            "window duration does not match statistics_window_ms");
    Require(!window.stations.empty(), "sparse window contains no station activity");

    std::map<uint32_t, std::vector<const StationStatisticsOutput*>> stationsByAccessPoint;
    std::optional<std::pair<uint32_t, uint32_t>> previousStation;
    for (const auto& station : window.stations)
    {
        const auto key = std::pair{station.accessPointId, station.stationIndex};
        const auto identity = stationInventory.find(key);
        Require(identity != stationInventory.end(), "window station is absent from inventory");
        Require(!previousStation || *previousStation < key,
                "window stations are duplicated or out of order");
        previousStation = key;
        ValidateStationIdentity(station, *identity->second);
        ValidateDefaultCategories(station.statistics, "window station");
        ValidateMetricFields(station.statistics.phyStats, "window station");
        Require(station.statistics.phyStats.averageTheoreticalPhyRateMbps.has_value() ||
                    *station.statistics.phyStats.contentionFraction > 0.0,
                "window emitted a station without PPDU or contention activity");
        stationsByAccessPoint[station.accessPointId].push_back(&station);
    }

    Require(window.accessPoints.size() == stationsByAccessPoint.size(),
            "window AP set does not match active station parents");
    std::size_t accessPointPosition = 0;
    for (const auto& accessPointIdentity : summary.accessPointInventory)
    {
        const auto activeStations = stationsByAccessPoint.find(accessPointIdentity.accessPointId);
        if (activeStations == stationsByAccessPoint.end())
        {
            continue;
        }
        const auto& accessPoint = window.accessPoints.at(accessPointPosition++);
        ValidateAccessPointIdentity(accessPoint, accessPointIdentity);
        ValidateAccessPointMetrics(accessPoint,
                                   activeStations->second,
                                   config.benchmark.stationCountPerBss);
    }
}

/** Validate dense overall entity shape and BSS arithmetic. */
void
ValidateOverall(const UnifiedExperimentSummary& summary,
                const SaturatedTcpConfig& config,
                const std::map<std::pair<uint32_t, uint32_t>, const ExperimentEntityIdentity*>&
                    stationInventory)
{
    Require(summary.overall.stations.size() == summary.stationInventory.size(),
            "overall station output is not dense");
    Require(summary.overall.accessPoints.size() == summary.accessPointInventory.size(),
            "overall AP output is not dense");

    std::map<uint32_t, std::vector<const StationStatisticsOutput*>> stationsByAccessPoint;
    for (std::size_t index = 0; index < summary.overall.stations.size(); ++index)
    {
        const auto& station = summary.overall.stations.at(index);
        const auto& identity = summary.stationInventory.at(index);
        ValidateStationIdentity(station, identity);
        Require(stationInventory.contains({station.accessPointId, station.stationIndex}),
                "overall station is absent from inventory");
        ValidateDefaultCategories(station.statistics, "overall station");
        ValidateMetricFields(station.statistics.phyStats, "overall station");
        stationsByAccessPoint[station.accessPointId].push_back(&station);
    }
    for (std::size_t index = 0; index < summary.overall.accessPoints.size(); ++index)
    {
        const auto& accessPoint = summary.overall.accessPoints.at(index);
        const auto& identity = summary.accessPointInventory.at(index);
        ValidateAccessPointIdentity(accessPoint, identity);
        ValidateAccessPointMetrics(accessPoint,
                                   stationsByAccessPoint.at(accessPoint.accessPointId),
                                   config.benchmark.stationCountPerBss);
    }
}

/** Validate public overall values against the sparse station windows. */
void
ValidateOverallAgainstWindows(const UnifiedExperimentSummary& summary)
{
    using StationKey = std::pair<uint32_t, uint32_t>;

    struct WindowMetric
    {
        double durationMs;              ///< Window duration in milliseconds.
        const PhyCategoryOutput* value; ///< Station PHY fields in that window.
    };

    std::map<StationKey, std::vector<WindowMetric>> metricsByStation;
    for (const auto& window : summary.windows)
    {
        for (const auto& station : window.stations)
        {
            metricsByStation[{station.accessPointId, station.stationIndex}].push_back(
                {window.windowDurationMs, &station.statistics.phyStats});
        }
    }

    for (const auto& station : summary.overall.stations)
    {
        const StationKey key{station.accessPointId, station.stationIndex};
        const auto windows = metricsByStation.find(key);

        long double expectedContention = 0.0L;
        std::optional<double> minimumTheoretical;
        std::optional<double> maximumTheoretical;
        std::optional<double> minimumPractical;
        std::optional<double> maximumPractical;
        if (windows != metricsByStation.end())
        {
            for (const auto& window : windows->second)
            {
                expectedContention +=
                    *window.value->contentionFraction * window.durationMs / 1000.0L;
                if (window.value->averageTheoreticalPhyRateMbps)
                {
                    const double theoretical = *window.value->averageTheoreticalPhyRateMbps;
                    const double practical = *window.value->averagePracticalPhyRateMbps;
                    minimumTheoretical = minimumTheoretical
                                             ? std::min(*minimumTheoretical, theoretical)
                                             : theoretical;
                    maximumTheoretical = maximumTheoretical
                                             ? std::max(*maximumTheoretical, theoretical)
                                             : theoretical;
                    minimumPractical =
                        minimumPractical ? std::min(*minimumPractical, practical) : practical;
                    maximumPractical =
                        maximumPractical ? std::max(*maximumPractical, practical) : practical;
                }
            }
        }

        const auto& overall = station.statistics.phyStats;
        Require(overall.averageTheoreticalPhyRateMbps.has_value() == minimumTheoretical.has_value(),
                "overall station rate presence does not match window PPDU observations");
        if (overall.averageTheoreticalPhyRateMbps)
        {
            Require(*overall.averageTheoreticalPhyRateMbps >=
                            *minimumTheoretical -
                                METRIC_TOLERANCE * std::max(1.0, *minimumTheoretical) &&
                        *overall.averageTheoreticalPhyRateMbps <=
                            *maximumTheoretical +
                                METRIC_TOLERANCE * std::max(1.0, *maximumTheoretical),
                    "overall theoretical PHY rate is outside its window range");
            Require(*overall.averagePracticalPhyRateMbps >=
                            *minimumPractical -
                                METRIC_TOLERANCE * std::max(1.0, *minimumPractical) &&
                        *overall.averagePracticalPhyRateMbps <=
                            *maximumPractical + METRIC_TOLERANCE * std::max(1.0, *maximumPractical),
                    "overall practical PHY rate is outside its window range");
        }
        Require(NearlyEqual(*overall.contentionFraction, static_cast<double>(expectedContention)),
                "overall contention does not reproduce sparse windows");
    }
}

/** Validate the complete benchmark summary before any output begins. */
void
ValidateBenchmarkSummary(const UnifiedExperimentSummary& summary, const SaturatedTcpConfig& config)
{
    ValidateSaturatedTcpConfig(config);
    Require(summary.statisticsWindowMs == config.statistics.windowMs,
            "statistics_window_ms does not match effective configuration");
    const auto stationInventory = ValidateInventory(summary, config);

    std::optional<uint64_t> previousWindowIndex;
    for (const auto& window : summary.windows)
    {
        Require(!previousWindowIndex || *previousWindowIndex < window.windowIndex,
                "windows are duplicated or out of order");
        previousWindowIndex = window.windowIndex;
        ValidateWindow(window, summary, config, stationInventory);
    }
    ValidateOverall(summary, config, stationInventory);
    ValidateOverallAgainstWindows(summary);
    ValidateFlags(summary.validation);
}

/** Remove a newly created partial output and throw its original failure. */
[[noreturn]] void
CleanupOwnedOutputAndThrow(std::ofstream& output,
                           const std::string& outputPath,
                           const std::string& failure)
{
    if (output.is_open())
    {
        output.clear();
        output.close();
    }
    std::error_code cleanupError;
    std::filesystem::remove(outputPath, cleanupError);
    if (cleanupError)
    {
        throw std::runtime_error(failure + "; additionally failed to remove partial output '" +
                                 outputPath + "': " + cleanupError.message());
    }
    throw std::runtime_error(failure);
}

} // namespace

namespace saturated_tcp_internal
{

void
WriteExclusiveJsonFile(const std::string& outputPath, const JsonBodyWriter& writeBody)
{
    if (!writeBody)
    {
        throw std::invalid_argument("saturated TCP JSON body writer must be set");
    }
    std::ofstream output(outputPath, std::ios::out | std::ios::noreplace);
    if (!output.is_open())
    {
        throw std::runtime_error("cannot exclusively create saturated TCP output: '" + outputPath +
                                 "'");
    }

    try
    {
        writeBody(output);
    }
    catch (const std::exception& error)
    {
        CleanupOwnedOutputAndThrow(output,
                                   outputPath,
                                   "failed to write saturated TCP output: '" + outputPath +
                                       "': " + error.what());
    }
    catch (...)
    {
        CleanupOwnedOutputAndThrow(output,
                                   outputPath,
                                   "failed to write saturated TCP output: '" + outputPath + "'");
    }
    if (!output)
    {
        CleanupOwnedOutputAndThrow(output,
                                   outputPath,
                                   "failed to write saturated TCP output: '" + outputPath + "'");
    }
    output.flush();
    if (!output)
    {
        CleanupOwnedOutputAndThrow(output,
                                   outputPath,
                                   "failed to flush saturated TCP output: '" + outputPath + "'");
    }
    output.close();
    if (output.fail())
    {
        CleanupOwnedOutputAndThrow(output,
                                   outputPath,
                                   "failed to close saturated TCP output: '" + outputPath + "'");
    }
}

} // namespace saturated_tcp_internal

void
WriteSaturatedMeasurementSemantics(JsonWriter& writer)
{
    writer.BeginObject();
    writer.Key("access_point_role");
    writer.Value("station-derived BSS aggregate");
    writer.Key("station_role");
    writer.Value("per-station transmitted PPDU detail");
    writer.Key("parent_child_duplication");
    writer.Value("intentional");
    writer.Key("phy_observation_scope");
    writer.Value("qualifying station-transmitted PPDUs");
    writer.Key("phy_rate_source");
    writer.Value("actual WifiTxVector and complete PPDU airtime");
    writer.Key("phy_practical_rate");
    writer.Value("qualifying PSDU bits per complete PPDU airtime");
    writer.Key("contention_fraction");
    writer.Value("unioned station EDCA waiting time per interval");
    writer.Key("sparse_window_absence");
    writer.Value("zero station PPDU and contention activity");
    writer.Key("undefined_derived_values");
    writer.Null();
    writer.EndObject();
}

void
WriteSaturatedTcpExperimentJson(std::ostream& output,
                                const UnifiedExperimentSummary& summary,
                                const SaturatedTcpConfig& config)
{
    ValidateBenchmarkSummary(summary, config);
    const ExperimentJsonSections sections{
        WriteSaturatedMeasurementSemantics,
        [&config](JsonWriter& writer) {
            WriteEffectiveSaturatedTcpConfigurationJson(writer, config);
        },
    };
    WriteExperimentHierarchyJson(output, summary, sections);
}

void
WriteSaturatedTcpExperimentJson(const std::string& outputPath,
                                const UnifiedExperimentSummary& summary,
                                const SaturatedTcpConfig& config)
{
    ValidateBenchmarkSummary(summary, config);
    const ExperimentJsonSections sections{
        WriteSaturatedMeasurementSemantics,
        [&config](JsonWriter& writer) {
            WriteEffectiveSaturatedTcpConfigurationJson(writer, config);
        },
    };
    saturated_tcp_internal::WriteExclusiveJsonFile(
        outputPath,
        [&summary, &sections](std::ostream& output) {
            WriteExperimentHierarchyJson(output, summary, sections);
        });
}

} // namespace ns3
