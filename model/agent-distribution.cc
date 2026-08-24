#include "agent-distribution.h"

#include "contention-aware-distribution-internal.h"
#include "llm-log.h"

#include "ns3/inet-socket-address.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace ns3
{

static LogComponent& g_log = llm_detail::GetAgentDistributionLog();

static std::vector<std::pair<double, double>>
GetTimeWindows(const std::vector<Operation>& operations)
{
    std::vector<std::pair<double, double>> windows;
    windows.reserve(operations.size());
    for (const auto& operation : operations)
    {
        windows.emplace_back(operation.startOffsetMs, operation.endMs);
    }
    return windows;
}

static std::map<std::string, int>
AssignAgentsToAccessPoints(const std::vector<AgentInfo>& agents,
                           int accessPointCount,
                           int slotMs = 50)
{
    NS_LOG_INFO("[AP-assign] Phase 1: Greedy assignment with " << slotMs << "ms slots");

    double firstTraceTimeMs = std::numeric_limits<double>::max();
    double lastTraceTimeMs = 0.0;
    for (const auto& agent : agents)
    {
        for (const auto& [startMs, endMs] : GetTimeWindows(agent.operations))
        {
            firstTraceTimeMs = std::min(firstTraceTimeMs, startMs);
            lastTraceTimeMs = std::max(lastTraceTimeMs, endMs);
        }
    }

    const int64_t slotCount = static_cast<int64_t>(
        std::ceil((lastTraceTimeMs - firstTraceTimeMs) / static_cast<double>(slotMs)));
    NS_LOG_INFO("[AP-assign] Time range: " << firstTraceTimeMs << "-" << lastTraceTimeMs << "ms, "
                                           << slotCount << " slots");

    struct AgentSlotMetrics
    {
        std::string key;
        std::set<int> activeSlots;
        int peakLoad{0};
        int64_t totalBytes{0};
    };

    std::vector<AgentSlotMetrics> metricsByAgent;
    metricsByAgent.reserve(agents.size());
    for (const auto& agent : agents)
    {
        AgentSlotMetrics metrics;
        metrics.key = agent.key;
        metrics.totalBytes = llm_detail::CalculateTotalBytes(agent.operations);

        std::map<int, int> operationCountBySlot;
        for (const auto& [startMs, endMs] : GetTimeWindows(agent.operations))
        {
            const int firstSlot = static_cast<int>(
                std::floor((startMs - firstTraceTimeMs) / static_cast<double>(slotMs)));
            const int endSlot = static_cast<int>(
                std::ceil((endMs - firstTraceTimeMs) / static_cast<double>(slotMs)));

            for (int slot = firstSlot; slot < endSlot && slot < static_cast<int>(slotCount); ++slot)
            {
                metrics.activeSlots.insert(slot);
                ++operationCountBySlot[slot];
            }
        }
        for (const auto& [slot, operationCount] : operationCountBySlot)
        {
            (void)slot;
            metrics.peakLoad = std::max(metrics.peakLoad, operationCount);
        }

        metricsByAgent.push_back(std::move(metrics));
    }

    std::sort(metricsByAgent.begin(),
              metricsByAgent.end(),
              [](const AgentSlotMetrics& left, const AgentSlotMetrics& right) {
                  if (left.peakLoad != right.peakLoad)
                  {
                      return left.peakLoad > right.peakLoad;
                  }
                  return left.totalBytes > right.totalBytes;
              });

    std::vector<std::vector<std::set<std::string>>> activeAgentsByAccessPointAndSlot(
        accessPointCount,
        std::vector<std::set<std::string>>(slotCount));
    std::map<std::string, int> accessPointByAgent;

    for (const auto& metrics : metricsByAgent)
    {
        int selectedAccessPoint = -1;
        int lowestPeakConcurrency = std::numeric_limits<int>::max();

        for (int accessPointIndex = 0; accessPointIndex < accessPointCount; ++accessPointIndex)
        {
            int candidatePeakConcurrency = 0;
            for (int slot : metrics.activeSlots)
            {
                const int candidateConcurrency =
                    static_cast<int>(
                        activeAgentsByAccessPointAndSlot[accessPointIndex][slot].size()) +
                    1;
                candidatePeakConcurrency = std::max(candidatePeakConcurrency, candidateConcurrency);
            }

            if (candidatePeakConcurrency < lowestPeakConcurrency)
            {
                lowestPeakConcurrency = candidatePeakConcurrency;
                selectedAccessPoint = accessPointIndex;
            }
        }

        accessPointByAgent[metrics.key] = selectedAccessPoint;
        for (int slot : metrics.activeSlots)
        {
            activeAgentsByAccessPointAndSlot[selectedAccessPoint][slot].insert(metrics.key);
        }

        NS_LOG_INFO("[AP-assign] Agent \""
                    << metrics.key << "\" peak=" << metrics.peakLoad
                    << " bytes=" << metrics.totalBytes << " -> AP" << selectedAccessPoint
                    << " (max concurrent on AP: " << lowestPeakConcurrency << ")");
    }

    for (int accessPointIndex = 0; accessPointIndex < accessPointCount; ++accessPointIndex)
    {
        int peakConcurrency = 0;
        for (int slot = 0; slot < static_cast<int>(slotCount); ++slot)
        {
            peakConcurrency = std::max(
                peakConcurrency,
                static_cast<int>(activeAgentsByAccessPointAndSlot[accessPointIndex][slot].size()));
        }
        NS_LOG_INFO("[AP-assign] AP" << accessPointIndex << ": " << accessPointByAgent.size()
                                     << " agents assigned, max concurrent: " << peakConcurrency);
    }

    return accessPointByAgent;
}

static void
AssignAgentsToStations(const std::vector<const AgentInfo*>& accessPointAgents,
                       int accessPointIndex,
                       int stationCount,
                       int maxAgentsPerStation,
                       std::map<std::string, Address>& stationAddressByAgent)
{
    NS_LOG_INFO("[Station-assign] AP" << accessPointIndex << ": assigning "
                                      << accessPointAgents.size() << " agents to " << stationCount
                                      << " stations (max " << maxAgentsPerStation << "/station)");

    std::vector<int> agentCountByStation(stationCount, 0);
    for (const auto* agent : accessPointAgents)
    {
        int selectedStation = 0;
        int fewestAgents = agentCountByStation[0];
        for (int stationIndex = 1; stationIndex < stationCount; ++stationIndex)
        {
            if (agentCountByStation[stationIndex] < fewestAgents)
            {
                fewestAgents = agentCountByStation[stationIndex];
                selectedStation = stationIndex;
            }
        }

        if (fewestAgents >= maxAgentsPerStation)
        {
            NS_LOG_WARN("[Station-assign] AP" << accessPointIndex << ": agent \"" << agent->key
                                              << "\" exceeds max agents per station ("
                                              << maxAgentsPerStation << ")");
            continue;
        }

        const std::string stationIp =
            "10.1." + std::to_string(accessPointIndex) + "." + std::to_string(2 + selectedStation);
        const Address stationAddress =
            InetSocketAddress(Ipv4Address(stationIp.c_str()), 9000 + selectedStation);

        stationAddressByAgent[agent->key] = stationAddress;
        ++agentCountByStation[selectedStation];

        NS_LOG_INFO("[Station-assign] AP"
                    << accessPointIndex << " agent \"" << agent->key << "\" -> station "
                    << selectedStation << " (IP: " << stationIp << " " << 9000 + selectedStation
                    << ", total on station: " << agentCountByStation[selectedStation] << ")");
    }

    int largestAgentCount = 0;
    int smallestAgentCount = std::numeric_limits<int>::max();
    int assignedAgentCount = 0;
    for (int stationIndex = 0; stationIndex < stationCount; ++stationIndex)
    {
        largestAgentCount = std::max(largestAgentCount, agentCountByStation[stationIndex]);
        smallestAgentCount = std::min(smallestAgentCount, agentCountByStation[stationIndex]);
        assignedAgentCount += agentCountByStation[stationIndex];
    }
    NS_LOG_INFO("[Station-assign] AP" << accessPointIndex << ": assigned " << assignedAgentCount
                                      << "/" << accessPointAgents.size()
                                      << " agents, per-station: min=" << smallestAgentCount
                                      << " max=" << largestAgentCount);
}

DistributionResult
DistributeAgents(const ParsedResult& parsedTrace,
                 int accessPointCount,
                 int stationsPerAccessPoint,
                 int maxAgentsPerStation)
{
    NS_LOG_INFO("[Distribute] Starting distribution: "
                << parsedTrace.agents.size() << " agents, " << accessPointCount << " APs, "
                << stationsPerAccessPoint << " stations/AP, max " << maxAgentsPerStation
                << " agents/station");

    DistributionResult distribution;
    distribution.apAgentMaps.resize(accessPointCount);
    distribution.apStationMaps.resize(accessPointCount);
    distribution.apAddresses.resize(accessPointCount);
    distribution.stationBases.resize(accessPointCount);

    // The AP device is assigned first and receives 10.1.<ap>.1.
    for (int accessPointIndex = 0; accessPointIndex < accessPointCount; ++accessPointIndex)
    {
        const std::string accessPointIp = "10.1." + std::to_string(accessPointIndex) + ".1";
        distribution.apAddresses[accessPointIndex] =
            InetSocketAddress(Ipv4Address(accessPointIp.c_str()), 10000);
        distribution.stationBases[accessPointIndex] =
            InetSocketAddress(Ipv4Address(accessPointIp.c_str()), 9000);
    }

    const std::map<std::string, int> accessPointByAgent =
        AssignAgentsToAccessPoints(parsedTrace.agents, accessPointCount);

    std::vector<std::vector<const AgentInfo*>> agentsByAccessPoint(accessPointCount);
    for (const auto& agent : parsedTrace.agents)
    {
        const auto assignment = accessPointByAgent.find(agent.key);
        if (assignment != accessPointByAgent.end())
        {
            agentsByAccessPoint[assignment->second].push_back(&agent);
            distribution.apAgentMaps[assignment->second][agent.key] = agent.operations;
        }
    }

    for (int accessPointIndex = 0; accessPointIndex < accessPointCount; ++accessPointIndex)
    {
        if (agentsByAccessPoint[accessPointIndex].empty())
        {
            NS_LOG_INFO("[Distribute] AP" << accessPointIndex << ": no agents");
            continue;
        }

        NS_LOG_INFO("[Distribute] Phase 2: AP" << accessPointIndex << " has "
                                               << agentsByAccessPoint[accessPointIndex].size()
                                               << " agents");

        AssignAgentsToStations(agentsByAccessPoint[accessPointIndex],
                               accessPointIndex,
                               stationsPerAccessPoint,
                               maxAgentsPerStation,
                               distribution.apStationMaps[accessPointIndex]);
    }

    NS_LOG_INFO("[Distribute] Distribution complete");
    return distribution;
}

} // namespace ns3
