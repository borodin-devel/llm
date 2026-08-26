#include "../logging/llm-log.h"
#include "contention-aware-distribution-internal.h"

#include "ns3/inet-socket-address.h"
#include "ns3/ipv4-address.h"

#include <algorithm>
#include <limits>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ns3::llm_detail
{

static LogComponent& g_log = GetContentionAwareDistributionLog();

/**
 * Assign agents belonging to one BSS to its stations.
 *
 * Low-contention mode minimizes newly active STA/slot pairs. Maximum-STA mode
 * first places mutually low-affinity seed agents on distinct stations, then
 * applies the same affinity-aware assignment. A zero agent cap is unlimited.
 */
void
AssignAgentsToStations(const std::vector<AgentActivity>& activities,
                       const std::vector<int>& bssAssignment,
                       int bss,
                       const ContentionAwareDistributionConfig& config,
                       std::map<std::string, Address>& stationMap)
{
    // Collect indices of agents assigned to this BSS.
    std::vector<int> agents;

    for (int agentIndex = 0; agentIndex < static_cast<int>(activities.size()); ++agentIndex)
    {
        if (bssAssignment[agentIndex] == bss)
        {
            agents.push_back(agentIndex);
        }
    }

    if (agents.empty())
    {
        NS_LOG_INFO("[STA assignment] BSS" << bss << ": no agents");

        return;
    }

    // Pairwise affinity counts shared UL slots and drives station clustering.
    std::unordered_map<int, std::vector<int>> agentsByUlSlot;

    for (int agentIndex : agents)
    {
        for (int slot : activities[agentIndex].uplinkSlots)
        {
            agentsByUlSlot[slot].push_back(agentIndex);
        }
    }

    std::vector<std::unordered_map<int, int>> pairwiseAffinity(activities.size());

    std::vector<int64_t> conflictScore(activities.size(), 0);

    for (const auto& [slot, slotAgents] : agentsByUlSlot)
    {
        (void)slot;

        const int64_t conflictContribution = static_cast<int64_t>(slotAgents.size()) - 1;

        for (int agentIndex : slotAgents)
        {
            conflictScore[agentIndex] += conflictContribution;
        }

        for (std::size_t i = 0; i < slotAgents.size(); ++i)
        {
            for (std::size_t j = i + 1; j < slotAgents.size(); ++j)
            {
                const int lhs = slotAgents[i];
                const int rhs = slotAgents[j];

                ++pairwiseAffinity[lhs][rhs];
                ++pairwiseAffinity[rhs][lhs];
            }
        }
    }

    const auto getPairwiseAffinity = [&](int lhs, int rhs) -> int {
        const auto entry = pairwiseAffinity[lhs].find(rhs);

        return entry == pairwiseAffinity[lhs].end() ? 0 : entry->second;
    };

    // Process the most problematic agents first.
    //
    // Tie-breaks make the output deterministic and give agents with more
    // activity / traffic slightly higher placement priority.
    std::sort(agents.begin(), agents.end(), [&](int lhs, int rhs) {
        if (conflictScore[lhs] != conflictScore[rhs])
        {
            return conflictScore[lhs] > conflictScore[rhs];
        }

        if (activities[lhs].uplinkSlots.size() != activities[rhs].uplinkSlots.size())
        {
            return activities[lhs].uplinkSlots.size() > activities[rhs].uplinkSlots.size();
        }

        if (activities[lhs].totalBytes != activities[rhs].totalBytes)
        {
            return activities[lhs].totalBytes > activities[rhs].totalBytes;
        }

        return activities[lhs].agent->key < activities[rhs].agent->key;
    });

    std::vector<int> agentCountPerStation(config.nStationsPerAp, 0);

    // Union of UL slots already active on each physical station.
    std::vector<std::unordered_set<int>> stationUlSlots(config.nStationsPerAp);

    // Agent indices already assigned to each physical station.
    std::vector<std::vector<int>> stationAgents(config.nStationsPerAp);

    // Chosen station by global activity index, used for final diagnostics.
    std::vector<int> stationOfAgent(activities.size(), -1);

    const auto calculateNewActiveSlots = [&](int agentIndex, int station) -> int {
        int newActiveSlots = 0;

        for (int slot : activities[agentIndex].uplinkSlots)
        {
            if (stationUlSlots[station].find(slot) == stationUlSlots[station].end())
            {
                ++newActiveSlots;
            }
        }

        return newActiveSlots;
    };

    const auto calculateStationAffinity = [&](int agentIndex, int station) -> int64_t {
        int64_t affinity = 0;

        for (int otherAgent : stationAgents[station])
        {
            affinity += getPairwiseAffinity(agentIndex, otherAgent);
        }

        return affinity;
    };

    const auto commitStationAssignment = [&](int agentIndex,
                                             int station,
                                             int newActiveSlots,
                                             int64_t stationAffinity,
                                             const char* phase) {
        stationOfAgent[agentIndex] = station;

        ++agentCountPerStation[station];
        stationAgents[station].push_back(agentIndex);

        for (int slot : activities[agentIndex].uplinkSlots)
        {
            stationUlSlots[station].insert(slot);
        }

        // Preserve the same addressing convention used by the existing
        // scenario:
        //
        //     AP      = 10.1.<bss>.1
        //     STA 0   = 10.1.<bss>.2
        //     STA 1   = 10.1.<bss>.3
        //     ...
        //
        // Station sink ports follow the existing 9000 + stationIndex rule.
        const std::string stationIp =
            "10.1." + std::to_string(bss) + "." + std::to_string(2 + station);

        stationMap[activities[agentIndex].agent->key] =
            InetSocketAddress(Ipv4Address(stationIp.c_str()), 9000 + station);

        NS_LOG_INFO("[STA assignment] BSS"
                    << bss << " agent=\"" << activities[agentIndex].agent->key << "\" -> STA"
                    << station << " ip=" << stationIp << " agentsOnSta="
                    << agentCountPerStation[station] << " newActiveUlSlots=" << newActiveSlots
                    << " affinityToSta=" << stationAffinity << " phase=" << phase);
    };

    NS_LOG_INFO("[STA assignment] BSS"
                << bss << ": agents=" << agents.size()
                << " availableStations=" << config.nStationsPerAp << " maxAgentsPerStation="
                << (config.maxAgentsPerStation == 0 ? std::string("unlimited")
                                                    : std::to_string(config.maxAgentsPerStation))
                << " lowContentionPriority=" << (config.lowContentionPriority ? "true" : "false"));

    // Maximum-STA mode chooses mutually low-affinity seeds first.
    std::unordered_set<int> seededAgents;

    if (!config.lowContentionPriority)
    {
        const int stationsToUse = std::min(config.nStationsPerAp, static_cast<int>(agents.size()));

        std::vector<int> seeds;
        seeds.reserve(stationsToUse);

        // The first seed is the highest-priority agent according to the normal
        // conflict ordering. Subsequent seeds are selected to represent other
        // low-overlap groups inside this BSS.
        seeds.push_back(agents.front());
        seededAgents.insert(agents.front());

        while (static_cast<int>(seeds.size()) < stationsToUse)
        {
            int bestSeed = -1;
            int bestMaxAffinity = std::numeric_limits<int>::max();
            int64_t bestTotalAffinity = std::numeric_limits<int64_t>::max();

            for (int candidate : agents)
            {
                if (seededAgents.find(candidate) != seededAgents.end())
                {
                    continue;
                }

                int maxAffinityToSeed = 0;
                int64_t totalAffinityToSeeds = 0;

                for (int seed : seeds)
                {
                    const int affinity = getPairwiseAffinity(candidate, seed);

                    maxAffinityToSeed = std::max(maxAffinityToSeed, affinity);

                    totalAffinityToSeeds += affinity;
                }

                // Primary: avoid choosing a seed strongly tied to any existing
                // seed. Secondary: minimize aggregate overlap with all seeds.
                // Remaining ties follow the already deterministic `agents`
                // order because we keep the first equivalent candidate.
                const bool isBetter = bestSeed < 0 ||

                                      maxAffinityToSeed < bestMaxAffinity ||

                                      (maxAffinityToSeed == bestMaxAffinity &&
                                       totalAffinityToSeeds < bestTotalAffinity);

                if (isBetter)
                {
                    bestSeed = candidate;
                    bestMaxAffinity = maxAffinityToSeed;
                    bestTotalAffinity = totalAffinityToSeeds;
                }
            }

            if (bestSeed < 0)
            {
                throw std::runtime_error("DistributeAgentsContentionAware: "
                                         "failed to choose a STA seed agent");
            }

            seeds.push_back(bestSeed);
            seededAgents.insert(bestSeed);
        }

        // Exactly one seed per STA guarantees maximum possible STA utilization.
        for (int station = 0; station < static_cast<int>(seeds.size()); ++station)
        {
            const int seed = seeds[station];

            commitStationAssignment(seed,
                                    station,
                                    static_cast<int>(activities[seed].uplinkSlots.size()),
                                    0,
                                    "seed");
        }
    }

    // Then minimize new active slots, maximize affinity, balance occupancy, and use STA ID.
    for (int agentIndex : agents)
    {
        if (seededAgents.find(agentIndex) != seededAgents.end())
        {
            continue;
        }

        int bestStation = -1;

        int bestNewActiveSlots = std::numeric_limits<int>::max();

        int64_t bestStationAffinity = std::numeric_limits<int64_t>::min();

        for (int station = 0; station < config.nStationsPerAp; ++station)
        {
            // Hard placement constraint when configured.
            // maxAgentsPerStation == 0 means unlimited.
            if (config.maxAgentsPerStation > 0 &&
                agentCountPerStation[station] >= config.maxAgentsPerStation)
            {
                continue;
            }

            const int newActiveSlots = calculateNewActiveSlots(agentIndex, station);

            const int64_t stationAffinity = calculateStationAffinity(agentIndex, station);

            const bool isBetter =
                bestStation < 0 ||

                newActiveSlots < bestNewActiveSlots ||

                (newActiveSlots == bestNewActiveSlots && stationAffinity > bestStationAffinity) ||

                (newActiveSlots == bestNewActiveSlots && stationAffinity == bestStationAffinity &&
                 agentCountPerStation[station] < agentCountPerStation[bestStation]) ||

                (newActiveSlots == bestNewActiveSlots && stationAffinity == bestStationAffinity &&
                 agentCountPerStation[station] == agentCountPerStation[bestStation] &&
                 station < bestStation);

            if (isBetter)
            {
                bestStation = station;
                bestNewActiveSlots = newActiveSlots;
                bestStationAffinity = stationAffinity;
            }
        }

        if (bestStation < 0)
        {
            throw std::runtime_error("DistributeAgentsContentionAware: "
                                     "no STA with free agent capacity");
        }

        commitStationAssignment(agentIndex,
                                bestStation,
                                bestNewActiveSlots,
                                bestStationAffinity,
                                "affinity");
    }

    int stationsUsed = 0;

    for (int count : agentCountPerStation)
    {
        if (count > 0)
        {
            ++stationsUsed;
        }
    }

    std::unordered_map<int, std::set<int>> activeStationsBySlot;

    for (int agentIndex : agents)
    {
        const int station = stationOfAgent[agentIndex];

        for (int slot : activities[agentIndex].uplinkSlots)
        {
            activeStationsBySlot[slot].insert(station);
        }
    }

    int maxActiveUlSta = 0;
    int64_t sumActiveUlStaSlots = 0;

    for (const auto& [slot, stations] : activeStationsBySlot)
    {
        const int activeStationCount = static_cast<int>(stations.size());

        maxActiveUlSta = std::max(maxActiveUlSta, activeStationCount);

        sumActiveUlStaSlots += activeStationCount;

        NS_LOG_DEBUG("[STA contention] BSS" << bss << " slot=" << slot
                                            << " activeUlSta=" << activeStationCount);
    }

    NS_LOG_INFO("[STA assignment] BSS" << bss << " summary: agents=" << agents.size()
                                       << " stationsUsed=" << stationsUsed
                                       << " maxActiveUlSta=" << maxActiveUlSta
                                       << " sumActiveUlStaSlots=" << sumActiveUlStaSlots);
}

} // namespace ns3::llm_detail
