#include "experiment-statistics-types.h"

#include <algorithm>
#include <stdexcept>
#include <tuple>

namespace ns3
{

void
SampleAccumulator::Add(double value)
{
    if (count == 0)
    {
        minimum = value;
        maximum = value;
    }
    else
    {
        minimum = std::min(minimum, value);
        maximum = std::max(maximum, value);
    }
    ++count;
    sum += value;
    sumSquares += static_cast<long double>(value) * value;
}

void
SampleAccumulator::Merge(const SampleAccumulator& other)
{
    if (other.count == 0)
    {
        return;
    }
    if (count == 0)
    {
        *this = other;
        return;
    }
    count += other.count;
    sum += other.sum;
    sumSquares += other.sumSquares;
    minimum = std::min(minimum, other.minimum);
    maximum = std::max(maximum, other.maximum);
}

void
ExperimentEntityRegistry::RegisterAccessPoint(uint32_t accessPointId,
                                              uint32_t nodeId,
                                              std::string nodeLabel,
                                              std::string ipv4)
{
    if (m_accessPointIndexes.contains(accessPointId))
    {
        throw std::invalid_argument("Duplicate access point identity");
    }
    if (m_nodeLocations.contains(nodeId))
    {
        throw std::invalid_argument("Duplicate node ID");
    }
    if (m_ipv4Locations.contains(ipv4))
    {
        throw std::invalid_argument("Duplicate IPv4 address");
    }

    m_accessPoints.push_back({ExperimentEntityKind::ACCESS_POINT,
                              accessPointId,
                              std::nullopt,
                              nodeId,
                              std::move(nodeLabel),
                              std::move(ipv4)});
    std::sort(m_accessPoints.begin(),
              m_accessPoints.end(),
              [](const auto& left, const auto& right) {
                  return left.accessPointId < right.accessPointId;
              });
    RebuildLookupIndexes();
}

void
ExperimentEntityRegistry::RegisterStation(uint32_t accessPointId,
                                          uint32_t stationIndex,
                                          uint32_t nodeId,
                                          std::string nodeLabel,
                                          std::string ipv4)
{
    const auto identity = std::make_pair(accessPointId, stationIndex);
    if (m_stationIndexes.contains(identity))
    {
        throw std::invalid_argument("Duplicate station identity");
    }
    if (m_nodeLocations.contains(nodeId))
    {
        throw std::invalid_argument("Duplicate node ID");
    }
    if (m_ipv4Locations.contains(ipv4))
    {
        throw std::invalid_argument("Duplicate IPv4 address");
    }

    m_stations.push_back({ExperimentEntityKind::STATION,
                          accessPointId,
                          stationIndex,
                          nodeId,
                          std::move(nodeLabel),
                          std::move(ipv4)});
    std::sort(m_stations.begin(), m_stations.end(), [](const auto& left, const auto& right) {
        return std::tie(left.accessPointId, left.stationIndex) <
               std::tie(right.accessPointId, right.stationIndex);
    });
    RebuildLookupIndexes();
}

const ExperimentEntityIdentity*
ExperimentEntityRegistry::FindByNodeId(uint32_t nodeId) const
{
    const auto iterator = m_nodeLocations.find(nodeId);
    if (iterator == m_nodeLocations.end())
    {
        return nullptr;
    }
    return iterator->second.isAccessPoint ? &m_accessPoints.at(iterator->second.index)
                                          : &m_stations.at(iterator->second.index);
}

const ExperimentEntityIdentity*
ExperimentEntityRegistry::FindByIpv4(std::string_view ipv4) const
{
    const auto iterator = m_ipv4Locations.find(ipv4);
    if (iterator == m_ipv4Locations.end())
    {
        return nullptr;
    }
    return iterator->second.isAccessPoint ? &m_accessPoints.at(iterator->second.index)
                                          : &m_stations.at(iterator->second.index);
}

const std::vector<ExperimentEntityIdentity>&
ExperimentEntityRegistry::GetAccessPoints() const
{
    return m_accessPoints;
}

const std::vector<ExperimentEntityIdentity>&
ExperimentEntityRegistry::GetStations() const
{
    return m_stations;
}

void
ExperimentEntityRegistry::RebuildLookupIndexes()
{
    m_accessPointIndexes.clear();
    m_stationIndexes.clear();
    m_nodeLocations.clear();
    m_ipv4Locations.clear();

    for (std::size_t index = 0; index < m_accessPoints.size(); ++index)
    {
        const auto& identity = m_accessPoints.at(index);
        m_accessPointIndexes.emplace(identity.accessPointId, index);
        const EntityLocation location{true, index};
        m_nodeLocations.emplace(identity.nodeId, location);
        m_ipv4Locations.emplace(identity.ipv4, location);
    }
    for (std::size_t index = 0; index < m_stations.size(); ++index)
    {
        const auto& identity = m_stations.at(index);
        m_stationIndexes.emplace(
            std::make_pair(identity.accessPointId, identity.stationIndex.value()),
            index);
        const EntityLocation location{false, index};
        m_nodeLocations.emplace(identity.nodeId, location);
        m_ipv4Locations.emplace(identity.ipv4, location);
    }
}

bool
ResolveExperimentWindow(int64_t relativeUs,
                        int64_t experimentDurationUs,
                        int64_t windowUs,
                        ExperimentWindowBounds& bounds)
{
    if (relativeUs < 0 || experimentDurationUs <= 0 || windowUs <= 0 ||
        relativeUs >= experimentDurationUs)
    {
        return false;
    }

    const int64_t startUs = relativeUs - relativeUs % windowUs;
    bounds = {static_cast<uint64_t>(relativeUs / windowUs),
              startUs,
              std::min(windowUs, experimentDurationUs - startUs)};
    return true;
}

std::vector<std::pair<uint64_t, int64_t>>
SplitExperimentInterval(int64_t relativeStartUs,
                        int64_t relativeEndUs,
                        int64_t experimentDurationUs,
                        int64_t windowUs)
{
    if (experimentDurationUs <= 0 || windowUs <= 0)
    {
        return {};
    }

    const int64_t clippedStartUs =
        std::min(std::max(relativeStartUs, int64_t{0}), experimentDurationUs);
    const int64_t clippedEndUs =
        std::min(std::max(relativeEndUs, int64_t{0}), experimentDurationUs);
    if (clippedStartUs >= clippedEndUs)
    {
        return {};
    }

    std::vector<std::pair<uint64_t, int64_t>> pieces;
    int64_t currentUs = clippedStartUs;
    while (currentUs < clippedEndUs)
    {
        ExperimentWindowBounds bounds{};
        if (!ResolveExperimentWindow(currentUs, experimentDurationUs, windowUs, bounds))
        {
            return {};
        }
        const int64_t remainingWindowUs = bounds.durationUs - (currentUs - bounds.startUs);
        const int64_t overlapUs = std::min(remainingWindowUs, clippedEndUs - currentUs);
        pieces.emplace_back(bounds.index, overlapUs);
        currentUs += overlapUs;
    }
    return pieces;
}

} // namespace ns3
