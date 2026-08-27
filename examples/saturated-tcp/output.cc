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
#include <tuple>
#include <utility>
#include <vector>

namespace ns3
{

namespace
{

constexpr double METRIC_TOLERANCE = 1e-9; ///< Relative benchmark validation tolerance.

void
Require(bool condition, const std::string& message)
{
    if (!condition)
    {
        throw std::invalid_argument("invalid saturated benchmark summary: " + message);
    }
}

bool
NearlyEqual(double left, double right)
{
    const double scale = std::max({1.0, std::abs(left), std::abs(right)});
    return std::abs(left - right) <= METRIC_TOLERANCE * scale;
}

void
ValidateOptionalNonnegative(const std::optional<double>& value, const std::string& name)
{
    if (value)
    {
        Require(std::isfinite(*value) && *value >= 0.0, name + " must be finite and non-negative");
    }
}

void
ClearBenchmarkFields(PhyCategoryOutput& phy)
{
    phy.dominantDataPhyRateMbps.reset();
    phy.dominantDataProfileShare.reset();
    phy.effectivePhyRateMbps.reset();
    phy.dataTxRateOverIntervalMbps.reset();
    phy.dataTxOpportunityGapFraction.reset();
    phy.dataTxProfile.clear();
    phy.meanDominantDataPhyRateMbps.reset();
    phy.meanEffectivePhyRateMbps.reset();
    phy.aggregateDataTxRateOverIntervalMbps.reset();
}

std::string
SerializeNonBenchmarkStatistics(EntityStatisticsOutput statistics)
{
    ClearBenchmarkFields(statistics.phyStats);
    std::ostringstream output;
    JsonWriter writer(output);
    writer.BeginObject();
    WriteEntityStatisticsJson(writer, statistics);
    writer.EndObject();
    writer.Finish();
    return output.str();
}

void
ValidateDefaultCategories(const EntityStatisticsOutput& statistics, const std::string& entity)
{
    static const std::string defaults = SerializeNonBenchmarkStatistics({});
    Require(SerializeNonBenchmarkStatistics(statistics) == defaults,
            entity + " contains non-default unrelated statistics");
}

void
ValidateStationMetrics(const PhyCategoryOutput& phy,
                       double intervalDurationMs,
                       const std::string& entity)
{
    Require(!phy.meanDominantDataPhyRateMbps && !phy.meanEffectivePhyRateMbps &&
                !phy.aggregateDataTxRateOverIntervalMbps,
            entity + " contains BSS aggregate fields");
    for (const auto* value : {&phy.dominantDataPhyRateMbps,
                              &phy.dominantDataProfileShare,
                              &phy.effectivePhyRateMbps,
                              &phy.dataTxRateOverIntervalMbps,
                              &phy.dataTxOpportunityGapFraction})
    {
        ValidateOptionalNonnegative(*value, entity + " station metric");
    }
    if (phy.dataTxProfile.empty())
    {
        Require(!phy.dominantDataPhyRateMbps && !phy.dominantDataProfileShare &&
                    !phy.effectivePhyRateMbps && phy.dataTxRateOverIntervalMbps &&
                    *phy.dataTxRateOverIntervalMbps == 0.0 && !phy.dataTxOpportunityGapFraction,
                entity + " empty profile does not use the null/null/null/zero/null shape");
        return;
    }
    Require(phy.dominantDataPhyRateMbps && phy.dominantDataProfileShare &&
                phy.effectivePhyRateMbps && phy.dataTxRateOverIntervalMbps &&
                phy.dataTxOpportunityGapFraction,
            entity + " populated profile has undefined derived fields");
    Require(*phy.dominantDataPhyRateMbps > 0.0, entity + " dominant data PHY rate is not positive");
    Require(*phy.dominantDataProfileShare > 0.0 && *phy.dominantDataProfileShare <= 1.0,
            entity + " dominant profile share is outside (0, 1]");
    Require(*phy.dataTxOpportunityGapFraction <= 1.0,
            entity + " opportunity gap is outside [0, 1]");

    double totalBytes = 0.0;
    double totalAirtimeUs = 0.0;
    double dominantBytes = -1.0;
    std::optional<std::tuple<uint16_t, uint8_t, uint8_t>> previous;
    for (const auto& profile : phy.dataTxProfile)
    {
        const std::tuple key{profile.channelWidthMhz, profile.nss, profile.mcs};
        Require((profile.channelWidthMhz == 20 || profile.channelWidthMhz == 40 ||
                 profile.channelWidthMhz == 80) &&
                    profile.nss > 0 && profile.mcs <= 11,
                entity + " profile width, NSS, or MCS is outside its range");
        Require(!previous || *previous < key, entity + " profiles are duplicated or out of order");
        previous = key;
        Require(std::isfinite(profile.transmittedPsduBytes) && profile.transmittedPsduBytes >= 0.0,
                entity + " profile bytes are invalid");
        Require(std::isfinite(profile.ppduAirtimeUs) && profile.ppduAirtimeUs >= 0.0,
                entity + " profile airtime is invalid");
        totalBytes += profile.transmittedPsduBytes;
        totalAirtimeUs += profile.ppduAirtimeUs;
        dominantBytes = std::max(dominantBytes, profile.transmittedPsduBytes);
    }
    Require(totalBytes > 0.0 && totalAirtimeUs > 0.0,
            entity + " populated profile has non-positive totals");
    const double expectedShare = dominantBytes / totalBytes;
    const double expectedEffective = totalBytes * 8.0 / totalAirtimeUs;
    const double intervalUs = intervalDurationMs * 1000.0;
    const double expectedIntervalRate = totalBytes * 8.0 / intervalUs;
    const double expectedGap = 1.0 - totalAirtimeUs / intervalUs;
    Require(NearlyEqual(*phy.dominantDataProfileShare, expectedShare),
            entity + " dominant share does not reproduce profile bytes");
    Require(NearlyEqual(*phy.effectivePhyRateMbps, expectedEffective),
            entity + " effective rate does not reproduce profile bytes and airtime");
    Require(NearlyEqual(*phy.dataTxRateOverIntervalMbps, expectedIntervalRate),
            entity + " interval rate does not reproduce profile bytes");
    Require(NearlyEqual(*phy.dataTxOpportunityGapFraction, expectedGap),
            entity + " opportunity gap does not reproduce profile airtime");
}

PhyCategoryOutput
ExpectedBssMetrics(const std::vector<const StationStatisticsOutput*>& stations)
{
    std::vector<PhyCategoryOutput> values;
    values.reserve(stations.size());
    for (const auto* station : stations)
    {
        values.push_back(station->statistics.phyStats);
    }
    return DeriveBssDataTxMetrics(values);
}

void
ValidateBssMetrics(const PhyCategoryOutput& phy,
                   const std::vector<const StationStatisticsOutput*>& stations,
                   const std::string& entity)
{
    Require(!phy.dominantDataPhyRateMbps && !phy.dominantDataProfileShare &&
                !phy.effectivePhyRateMbps && !phy.dataTxRateOverIntervalMbps &&
                !phy.dataTxOpportunityGapFraction && phy.dataTxProfile.empty(),
            entity + " contains station-role fields");
    ValidateOptionalNonnegative(phy.meanDominantDataPhyRateMbps, entity + " dominant mean");
    ValidateOptionalNonnegative(phy.meanEffectivePhyRateMbps, entity + " effective mean");
    ValidateOptionalNonnegative(phy.aggregateDataTxRateOverIntervalMbps,
                                entity + " aggregate interval rate");
    const auto expected = ExpectedBssMetrics(stations);
    const auto compare = [&entity](const auto& actual, const auto& wanted, const char* name) {
        Require(actual.has_value() == wanted.has_value(),
                entity + " " + name + " presence differs");
        if (actual)
        {
            Require(NearlyEqual(*actual, *wanted), entity + " " + name + " differs");
        }
    };
    compare(phy.meanDominantDataPhyRateMbps, expected.meanDominantDataPhyRateMbps, "dominant mean");
    compare(phy.meanEffectivePhyRateMbps, expected.meanEffectivePhyRateMbps, "effective mean");
    compare(phy.aggregateDataTxRateOverIntervalMbps,
            expected.aggregateDataTxRateOverIntervalMbps,
            "aggregate interval rate");
}

void
ValidateAccessPointIdentity(const AccessPointStatisticsOutput& output,
                            const ExperimentEntityIdentity& identity)
{
    Require(output.accessPointId == identity.accessPointId && output.nodeId == identity.nodeId &&
                output.nodeLabel == identity.nodeLabel && output.ipv4 == identity.ipv4,
            "access point output does not match inventory");
}

void
ValidateStationIdentity(const StationStatisticsOutput& output,
                        const ExperimentEntityIdentity& identity)
{
    Require(output.accessPointId == identity.accessPointId &&
                output.stationIndex == identity.stationIndex && output.nodeId == identity.nodeId &&
                output.nodeLabel == identity.nodeLabel && output.ipv4 == identity.ipv4,
            "station output does not match inventory");
}

void
ValidateFlags(const ExperimentValidationOutput& validation)
{
    Require(validation.entityInventoryReferencesValid, "entity inventory references are invalid");
    Require(validation.appAgentTotalsConsistent, "application agent totals are inconsistent");
    Require(validation.appPeerTotalsConsistent, "application peer totals are inconsistent");
    Require(validation.macPeerTotalsConsistent, "MAC peer totals are inconsistent");
    Require(validation.phyPeerTotalsConsistent, "PHY peer totals are inconsistent");
    Require(validation.apStationSenderTotalsConsistent,
            "AP/station sender totals are inconsistent");
    Require(validation.overallMatchesWindows, "overall does not match merged windows");
    Require(validation.uniquePhyPayloadWithinTaggedPayload,
            "unique PHY payload exceeds tagged payload");
}

using StationIdentityMap = std::map<std::pair<uint32_t, uint32_t>, const ExperimentEntityIdentity*>;

StationIdentityMap
ValidateInventory(const UnifiedExperimentSummary& summary, const SaturatedTcpConfig& config)
{
    Require(summary.accessPointInventory.size() == 3,
            "benchmark inventory must contain exactly three access points");
    Require(summary.stationInventory.size() == 3 * config.benchmark.stationCountPerBss,
            "benchmark station inventory size differs from configuration");
    std::set<uint32_t> nodeIds;
    std::set<std::string> addresses;
    for (uint32_t index = 0; index < 3; ++index)
    {
        const auto& identity = summary.accessPointInventory.at(index);
        Require(identity.kind == ExperimentEntityKind::ACCESS_POINT &&
                    identity.accessPointId == index && !identity.stationIndex,
                "access point inventory is out of order");
        Require(nodeIds.insert(identity.nodeId).second && addresses.insert(identity.ipv4).second,
                "inventory contains a duplicate AP identity");
    }
    StationIdentityMap stations;
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
                    "station inventory is out of order");
            Require(nodeIds.insert(identity.nodeId).second &&
                        addresses.insert(identity.ipv4).second,
                    "inventory contains a duplicate station identity");
            stations.emplace(std::pair{accessPointId, stationIndex}, &identity);
        }
    }
    return stations;
}

void
ValidateWindow(const ExperimentWindowOutput& window,
               const UnifiedExperimentSummary& summary,
               const StationIdentityMap& inventory)
{
    Require(window.windowIndex < 1000 / summary.statisticsWindowMs,
            "window index exceeds the one-second epoch");
    Require(NearlyEqual(window.windowStartMs,
                        static_cast<double>(window.windowIndex) * summary.statisticsWindowMs) &&
                NearlyEqual(window.windowDurationMs, summary.statisticsWindowMs),
            "window position or duration is invalid");
    Require(!window.stations.empty(), "sparse window contains no station data profile");
    std::map<uint32_t, std::vector<const StationStatisticsOutput*>> byAccessPoint;
    std::optional<std::pair<uint32_t, uint32_t>> previous;
    for (const auto& station : window.stations)
    {
        const std::pair key{station.accessPointId, station.stationIndex};
        const auto identity = inventory.find(key);
        Require(identity != inventory.end() && (!previous || *previous < key),
                "window stations are absent, duplicated, or out of order");
        previous = key;
        ValidateStationIdentity(station, *identity->second);
        ValidateDefaultCategories(station.statistics, "window station");
        ValidateStationMetrics(station.statistics.phyStats,
                               window.windowDurationMs,
                               "window station");
        Require(!station.statistics.phyStats.dataTxProfile.empty(),
                "sparse window contains an inactive station");
        byAccessPoint[station.accessPointId].push_back(&station);
    }
    Require(window.accessPoints.size() == byAccessPoint.size(),
            "window BSS parents do not match active stations");
    std::size_t position = 0;
    for (const auto& identity : summary.accessPointInventory)
    {
        const auto stations = byAccessPoint.find(identity.accessPointId);
        if (stations == byAccessPoint.end())
        {
            continue;
        }
        const auto& accessPoint = window.accessPoints.at(position++);
        ValidateAccessPointIdentity(accessPoint, identity);
        ValidateDefaultCategories(accessPoint.statistics, "window BSS");
        ValidateBssMetrics(accessPoint.statistics.phyStats, stations->second, "window BSS");
    }
}

void
ValidateOverall(const UnifiedExperimentSummary& summary, const StationIdentityMap& inventory)
{
    Require(summary.overall.stations.size() == summary.stationInventory.size(),
            "overall station output is not dense");
    Require(summary.overall.accessPoints.size() == summary.accessPointInventory.size(),
            "overall BSS output is not dense");
    std::map<uint32_t, std::vector<const StationStatisticsOutput*>> byAccessPoint;
    for (std::size_t index = 0; index < summary.overall.stations.size(); ++index)
    {
        const auto& station = summary.overall.stations.at(index);
        ValidateStationIdentity(station, summary.stationInventory.at(index));
        Require(inventory.contains({station.accessPointId, station.stationIndex}),
                "overall station is absent from inventory");
        ValidateDefaultCategories(station.statistics, "overall station");
        ValidateStationMetrics(station.statistics.phyStats, 1000.0, "overall station");
        byAccessPoint[station.accessPointId].push_back(&station);
    }
    for (std::size_t index = 0; index < summary.overall.accessPoints.size(); ++index)
    {
        const auto& accessPoint = summary.overall.accessPoints.at(index);
        ValidateAccessPointIdentity(accessPoint, summary.accessPointInventory.at(index));
        ValidateDefaultCategories(accessPoint.statistics, "overall BSS");
        ValidateBssMetrics(accessPoint.statistics.phyStats,
                           byAccessPoint.at(accessPoint.accessPointId),
                           "overall BSS");
    }
}

void
ValidateOverallProfiles(const UnifiedExperimentSummary& summary)
{
    using StationKey = std::pair<uint32_t, uint32_t>;
    using ProfileKey = std::tuple<uint16_t, uint8_t, uint8_t>;
    std::map<StationKey, std::map<ProfileKey, DataTxProfileOutput>> merged;
    for (const auto& window : summary.windows)
    {
        for (const auto& station : window.stations)
        {
            auto& profiles = merged[{station.accessPointId, station.stationIndex}];
            for (const auto& profile : station.statistics.phyStats.dataTxProfile)
            {
                auto& total = profiles[{profile.channelWidthMhz, profile.nss, profile.mcs}];
                total.channelWidthMhz = profile.channelWidthMhz;
                total.nss = profile.nss;
                total.mcs = profile.mcs;
                total.transmittedPsduBytes += profile.transmittedPsduBytes;
                total.ppduAttemptCount += profile.ppduAttemptCount;
                total.ppduAirtimeUs += profile.ppduAirtimeUs;
            }
        }
    }
    for (const auto& station : summary.overall.stations)
    {
        const auto expected = merged.find({station.accessPointId, station.stationIndex});
        const auto& actual = station.statistics.phyStats.dataTxProfile;
        const std::size_t expectedSize = expected == merged.end() ? 0 : expected->second.size();
        Require(actual.size() == expectedSize,
                "overall station profile keys do not reproduce sparse windows");
        if (expected == merged.end())
        {
            continue;
        }
        std::size_t index = 0;
        for (const auto& [key, wanted] : expected->second)
        {
            const auto& value = actual.at(index++);
            Require(std::tuple{value.channelWidthMhz, value.nss, value.mcs} == key &&
                        NearlyEqual(value.transmittedPsduBytes, wanted.transmittedPsduBytes) &&
                        value.ppduAttemptCount == wanted.ppduAttemptCount &&
                        NearlyEqual(value.ppduAirtimeUs, wanted.ppduAirtimeUs),
                    "overall station profile values do not reproduce sparse windows");
        }
    }
}

void
ValidateBenchmarkSummary(const UnifiedExperimentSummary& summary, const SaturatedTcpConfig& config)
{
    ValidateSaturatedTcpConfig(config);
    Require(summary.statisticsWindowMs == config.statistics.windowMs,
            "statistics_window_ms does not match configuration");
    const auto inventory = ValidateInventory(summary, config);
    std::optional<uint64_t> previousWindow;
    for (const auto& window : summary.windows)
    {
        Require(!previousWindow || *previousWindow < window.windowIndex,
                "windows are duplicated or out of order");
        previousWindow = window.windowIndex;
        ValidateWindow(window, summary, inventory);
    }
    ValidateOverall(summary, inventory);
    ValidateOverallProfiles(summary);
    ValidateFlags(summary.validation);
}

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
    writer.Value("per-station transmitted data PPDU detail");
    writer.Key("parent_child_duplication");
    writer.Value("intentional");
    writer.Key("phy_observation_scope");
    writer.Value("qualifying station-transmitted unicast data PPDUs");
    writer.Key("phy_rate_source");
    writer.Value("actual fixed-invariant WifiTxVector NSS and MCS");
    writer.Key("effective_phy_rate");
    writer.Value("transmitted data PSDU bits per data PPDU airtime");
    writer.Key("data_tx_rate_over_interval");
    writer.Value("transmitted data PSDU bits per statistics interval");
    writer.Key("data_tx_opportunity_gap");
    writer.Value("time outside station data PPDU airtime");
    writer.Key("sparse_window_absence");
    writer.Value("zero station data profile activity");
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
