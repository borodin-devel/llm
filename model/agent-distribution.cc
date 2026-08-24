// model/agent-distribution.cc
//
// Agent Distribution Module - Implementation
// Greedy AP assignment (50ms slots) + greedy station assignment
//
// Implements: DistributeAgents()
// ParseJsonFile() is implemented in json_parser.cpp
//

#include "agent-distribution.h"

#include "contention-aware-distribution-internal.h"
#include "llm-log.h"

#include "ns3/inet-socket-address.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <map>
#include <utility>
#include <climits>
#include <limits>

namespace ns3
{

static LogComponent& g_log = llm_detail::GetAgentDistributionLog();

// ============================================================================
// Internal helpers
// ============================================================================

static std::vector<std::pair<double, double>>
GetTimeWindows(const std::vector<Operation>& ops)
{
    std::vector<std::pair<double, double>> windows;
    for (const auto& op : ops)
    {
        windows.emplace_back(op.startOffsetMs, op.endMs);
    }
    return windows;
}

// ============================================================================
// Phase 1: Greedy AP Assignment (50ms slots)
// ============================================================================

static std::map<std::string, int>
GreedyAssignAPs(const std::vector<AgentInfo>& agents,
                int nAp,
                int slotMs = 50)
{
    NS_LOG_INFO("[AP-assign] Phase 1: Greedy assignment with " << slotMs << "ms slots");

    // Find time range without truncating fractional milliseconds.
    double tMin = std::numeric_limits<double>::max();
    double tMax = 0.0;
    for (const auto& agent : agents)
    {
        for (const auto& [s, e] : GetTimeWindows(agent.operations))
        {
            tMin = std::min(tMin, s);
            tMax = std::max(tMax, e);
        }
    }

    const int64_t nSlots = static_cast<int64_t>(
        std::ceil((tMax - tMin) / static_cast<double>(slotMs)));
    NS_LOG_INFO("[AP-assign] Time range: " << tMin << "-" << tMax
               << "ms, " << nSlots << " slots");

    // For each agent, compute active slots and peakLoad
    struct AgentSlotInfo
    {
        std::string key;
        std::set<int> activeSlots;
        int peakLoad;
        int64_t totalBytes;
    };

    std::vector<AgentSlotInfo> slotInfos;
    for (const auto& agent : agents)
    {
        AgentSlotInfo info;
        info.key = agent.key;
        info.totalBytes = llm_detail::CalculateTotalBytes(agent.operations);

        std::map<int, int> slotCounts;
        for (const auto& [s, e] : GetTimeWindows(agent.operations))
        {
            const int firstSlot = static_cast<int>(
                std::floor((s - tMin) / static_cast<double>(slotMs)));
            const int endSlot = static_cast<int>(
                std::ceil((e - tMin) / static_cast<double>(slotMs)));

            for (int slot = firstSlot;
                 slot < endSlot && slot < static_cast<int>(nSlots);
                 ++slot)
            {
                info.activeSlots.insert(slot);
                slotCounts[slot]++;
            }
        }
        info.peakLoad = 0;
        for (const auto& [slot, count] : slotCounts)
        {
            info.peakLoad = std::max(info.peakLoad, count);
        }

        slotInfos.push_back(info);
    }

    // Sort by peakLoad descending, then by totalBytes descending
    std::sort(slotInfos.begin(), slotInfos.end(),
              [](const AgentSlotInfo& a, const AgentSlotInfo& b) {
                  if (a.peakLoad != b.peakLoad)
                      return a.peakLoad > b.peakLoad;
                  return a.totalBytes > b.totalBytes;
              });

    // Track per-AP per-slot active agents
    std::vector<std::vector<std::set<std::string>>> apSlotAgents(nAp,
                            std::vector<std::set<std::string>>(nSlots));
    std::map<std::string, int> assignment;

    for (const auto& info : slotInfos)
    {
        int bestAp = -1;
        int bestMax = INT32_MAX;

        for (int ap = 0; ap < nAp; ap++)
        {
            int maxConcurrent = 0;
            for (int slot : info.activeSlots)
            {
                int newCount = (int)apSlotAgents[ap][slot].size() + 1;
                maxConcurrent = std::max(maxConcurrent, newCount);
            }

            if (maxConcurrent < bestMax)
            {
                bestMax = maxConcurrent;
                bestAp = ap;
            }
        }

        assignment[info.key] = bestAp;
        for (int slot : info.activeSlots)
        {
            apSlotAgents[bestAp][slot].insert(info.key);
        }

        NS_LOG_INFO("[AP-assign] Agent \"" << info.key
               << "\" peak=" << info.peakLoad
               << " bytes=" << info.totalBytes
               << " -> AP" << bestAp
               << " (max concurrent on AP: " << bestMax << ")");
    }

    // Print per-AP stats
    for (int ap = 0; ap < nAp; ap++)
    {
        int maxConcurrent = 0;
        int64_t totalBytes = 0;
        for (const auto& [key, assignedAp] : assignment)
        {
            if (assignedAp == ap)
            {
                for (const auto& agent : agents)
                {
                    if (agent.key == key)
                    {
                        totalBytes += llm_detail::CalculateTotalBytes(agent.operations);
                        break;
                    }
                }
            }
        }
        for (int slot = 0; slot < (int)nSlots; slot++)
        {
            maxConcurrent = std::max(maxConcurrent, (int)apSlotAgents[ap][slot].size());
        }
        NS_LOG_INFO("[AP-assign] AP" << ap << ": "
               << assignment.size() << " agents assigned, "
               << "max concurrent: " << maxConcurrent);
    }

    return assignment;
}

// ============================================================================
// Phase 2: Greedy Station Assignment (per AP)
// ============================================================================

static void
AssignStationsToAgents(
    const std::vector<const AgentInfo*>& agents,
    int ap,
    int nStations,
    int maxAgentsPerStation,
    std::map<std::string, Address>& stationMap)
{
    NS_LOG_INFO("[Station-assign] AP" << ap << ": assigning "
               << agents.size() << " agents to " << nStations
               << " stations (max " << maxAgentsPerStation << "/station)");

    // Track agents per station
    std::vector<int> stationAgentCount(nStations, 0);

    // Assign agents round-robin to stations with fewest agents
    for (const auto* agent : agents)
    {
        // Find station with minimum agents
        int bestStation = 0;
        int minCount = stationAgentCount[0];
        for (int s = 1; s < nStations; s++)
        {
            if (stationAgentCount[s] < minCount)
            {
                minCount = stationAgentCount[s];
                bestStation = s;
            }
        }

        if (minCount >= maxAgentsPerStation)
        {
            NS_LOG_WARN("[Station-assign] AP" << ap
                       << ": agent \"" << agent->key
                       << "\" exceeds max agents per station ("
                       << maxAgentsPerStation << ")");
            continue;
        }

        // Create station address: 10.1.ap.(2 + stationIndex)
        std::string stationIpStr = "10.1." + std::to_string(ap) + "." + std::to_string(2 + bestStation);
        Address stationAddr = InetSocketAddress(Ipv4Address(stationIpStr.c_str()), 9000 + bestStation);

        stationMap[agent->key] = stationAddr;
        stationAgentCount[bestStation]++;

        NS_LOG_INFO("[Station-assign] AP" << ap
                   << " agent \"" << agent->key
                   << "\" -> station " << bestStation
                   << " (IP: " << stationIpStr << " " << (9000 + bestStation)
                   << ", total on station: " << stationAgentCount[bestStation] << ")");
    }

    // Print summary
    int maxOnAnyStation = 0;
    int minOnAnyStation = INT32_MAX;
    int totalAssigned = 0;
    for (int s = 0; s < nStations; s++)
    {
        maxOnAnyStation = std::max(maxOnAnyStation, stationAgentCount[s]);
        minOnAnyStation = std::min(minOnAnyStation, stationAgentCount[s]);
        totalAssigned += stationAgentCount[s];
    }
    NS_LOG_INFO("[Station-assign] AP" << ap
               << ": assigned " << totalAssigned << "/" << agents.size()
               << " agents, per-station: min=" << minOnAnyStation
               << " max=" << maxOnAnyStation);
}

// ============================================================================
// DistributeAgents (API)
// ============================================================================

DistributionResult
DistributeAgents(const ParsedResult& parsed,
                 int nAp,
                 int nStationsPerAp,
                 int maxAgentsPerStation)
{
    NS_LOG_INFO("[Distribute] Starting distribution: "
               << parsed.agents.size() << " agents, "
               << nAp << " APs, "
               << nStationsPerAp << " stations/AP, "
               << "max " << maxAgentsPerStation << " agents/station");

    // Initialize result
    DistributionResult result;
    result.apAgentMaps.resize(nAp);
    result.apStationMaps.resize(nAp);
    result.apAddresses.resize(nAp);
    result.stationBases.resize(nAp);

    // The AP device is assigned before station devices, therefore
    // every AP receives the first host address: 10.1.<ap>.1.
    for (int ap = 0; ap < nAp; ap++)
    {
        std::string ipStr = "10.1." + std::to_string(ap) + ".1";
        result.apAddresses[ap] = InetSocketAddress(Ipv4Address(ipStr.c_str()), 10000);
        result.stationBases[ap] = InetSocketAddress(Ipv4Address(ipStr.c_str()), 9000);
    }

    // Phase 1: AP assignment
    std::map<std::string, int> apAssignment = GreedyAssignAPs(parsed.agents, nAp);

    // Group agents by AP
    std::vector<std::vector<const AgentInfo*>> apAgents(nAp);
    for (const auto& agent : parsed.agents)
    {
        auto it = apAssignment.find(agent.key);
        if (it != apAssignment.end())
        {
            apAgents[it->second].push_back(&agent);
            result.apAgentMaps[it->second][agent.key] = agent.operations;
        }
    }

    // Phase 2: Station assignment per AP
    for (int ap = 0; ap < nAp; ap++)
    {
        if (apAgents[ap].empty())
        {
            NS_LOG_INFO("[Distribute] AP" << ap << ": no agents");
            continue;
        }

        NS_LOG_INFO("[Distribute] Phase 2: AP" << ap
                   << " has " << apAgents[ap].size() << " agents");

        AssignStationsToAgents(apAgents[ap], ap,
                               nStationsPerAp,
                               maxAgentsPerStation,
                               result.apStationMaps[ap]);

    }

    NS_LOG_INFO("[Distribute] Distribution complete");
    return result;
}

};
