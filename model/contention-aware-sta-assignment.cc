#include "contention-aware-distribution-internal.h"

#include "llm-log.h"

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
 * There are two policies.
 *
 *
 * lowContentionPriority == true
 * --------------------------------
 *
 * Minimize the number of newly active UL STA/time-slot pairs.
 *
 * Example:
 *
 *     Agent A uses UL slots: {10, 11, 12}
 *
 *     STA0 already active in: {10, 11, 12, 20}
 *     STA1 already active in: {30, 31}
 *
 * Putting A on STA0 adds:
 *
 *     0 new active STA slots
 *
 * Putting A on STA1 adds:
 *
 *     3 new active STA slots
 *
 * Therefore STA0 is strongly preferred.
 *
 * This is the core of the low-contention mode: multiple overlapping
 * application-level agents can share one physical STA. A positive
 * maxAgentsPerStation imposes a hard limit; zero means unlimited.
 *
 *
 * lowContentionPriority == false
 * ---------------------------------
 *
 * Use as many physical STAs as possible, but do not seed them with mutually
 * conflicting agents when avoidable. One low-affinity seed is selected for
 * every STA that must be used. Remaining agents are then attached to the STA
 * that adds the fewest new active UL slots and has the strongest pairwise
 * overlap with agents already placed there.
 *
 * This preserves maximum STA utilization while still grouping application
 * conflicts behind as few MAC contenders as the capacity constraints allow.
 */
void
AssignAgentsToStations(
    const std::vector<AgentActivity>& activities,
    const std::vector<int>& bssAssignment,
    int bss,
    const ContentionAwareDistributionConfig& config,
    std::map<std::string, Address>& stationMap)
{
    // Collect indices of agents assigned to this BSS.
    std::vector<int> agents;

    for (int agentIndex = 0;
         agentIndex < static_cast<int>(activities.size());
         ++agentIndex)
    {
        if (bssAssignment[agentIndex] == bss)
        {
            agents.push_back(agentIndex);
        }
    }

    if (agents.empty())
    {
        NS_LOG_INFO(
            "[STA assignment] BSS"
            << bss
            << ": no agents");

        return;
    }

    // ------------------------------------------------------------------------
    // Build the intra-BSS weighted conflict graph.
    //
    // pairwiseAffinity[a][b] is the number of UL slots shared by agents a and
    // b. Unlike the scalar conflictScore, this retains information about WHO
    // conflicts with whom, which is exactly what STA clustering needs.
    // ------------------------------------------------------------------------

    std::unordered_map<int, std::vector<int>> agentsByUlSlot;

    for (int agentIndex : agents)
    {
        for (int slot : activities[agentIndex].uplinkSlots)
        {
            agentsByUlSlot[slot].push_back(agentIndex);
        }
    }

    std::vector<std::unordered_map<int, int>> pairwiseAffinity(
        activities.size());

    std::vector<int64_t> conflictScore(activities.size(), 0);

    for (const auto& [slot, slotAgents] : agentsByUlSlot)
    {
        (void)slot;

        const int64_t conflictContribution =
            static_cast<int64_t>(slotAgents.size()) - 1;

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

    const auto getPairwiseAffinity =
        [&](int lhs, int rhs) -> int
        {
            const auto entry = pairwiseAffinity[lhs].find(rhs);

            return entry == pairwiseAffinity[lhs].end()
                       ? 0
                       : entry->second;
        };

    // Process the most problematic agents first.
    //
    // Tie-breaks make the output deterministic and give agents with more
    // activity / traffic slightly higher placement priority.
    std::sort(
        agents.begin(),
        agents.end(),
        [&](int lhs, int rhs)
        {
            if (conflictScore[lhs] != conflictScore[rhs])
            {
                return conflictScore[lhs] > conflictScore[rhs];
            }

            if (activities[lhs].uplinkSlots.size() !=
                activities[rhs].uplinkSlots.size())
            {
                return activities[lhs].uplinkSlots.size() >
                       activities[rhs].uplinkSlots.size();
            }

            if (activities[lhs].totalBytes !=
                activities[rhs].totalBytes)
            {
                return activities[lhs].totalBytes >
                       activities[rhs].totalBytes;
            }

            return activities[lhs].agent->key <
                   activities[rhs].agent->key;
        });

    // ------------------------------------------------------------------------
    // Current station state.
    // ------------------------------------------------------------------------

    std::vector<int> agentCountPerStation(
        config.nStationsPerAp,
        0);

    /**
     * Union of all UL slots belonging to agents already assigned to each STA.
     *
     * This set directly represents whether a physical station is already
     * considered active in a given time window.
     */
    std::vector<std::unordered_set<int>> stationUlSlots(
        config.nStationsPerAp);

    /** Agents already assigned to every physical STA. */
    std::vector<std::vector<int>> stationAgents(
        config.nStationsPerAp);

    /**
     * Used only for final diagnostics.
     *
     * Index is the global AgentActivity index, value is the chosen STA index.
     */
    std::vector<int> stationOfAgent(
        activities.size(),
        -1);

    const auto calculateNewActiveSlots =
        [&](int agentIndex, int station) -> int
        {
            int newActiveSlots = 0;

            for (int slot : activities[agentIndex].uplinkSlots)
            {
                if (stationUlSlots[station].find(slot) ==
                    stationUlSlots[station].end())
                {
                    ++newActiveSlots;
                }
            }

            return newActiveSlots;
        };

    const auto calculateStationAffinity =
        [&](int agentIndex, int station) -> int64_t
        {
            int64_t affinity = 0;

            for (int otherAgent : stationAgents[station])
            {
                affinity += getPairwiseAffinity(agentIndex, otherAgent);
            }

            return affinity;
        };

    const auto commitStationAssignment =
        [&](int agentIndex,
            int station,
            int newActiveSlots,
            int64_t stationAffinity,
            const char* phase)
        {
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
                "10.1." +
                std::to_string(bss) +
                "." +
                std::to_string(2 + station);

            stationMap[activities[agentIndex].agent->key] =
                InetSocketAddress(
                    Ipv4Address(stationIp.c_str()),
                    9000 + station);

            NS_LOG_INFO(
                "[STA assignment] BSS"
                << bss
                << " agent=\""
                << activities[agentIndex].agent->key
                << "\" -> STA"
                << station
                << " ip="
                << stationIp
                << " agentsOnSta="
                << agentCountPerStation[station]
                << " newActiveUlSlots="
                << newActiveSlots
                << " affinityToSta="
                << stationAffinity
                << " phase="
                << phase);
        };

    NS_LOG_INFO(
        "[STA assignment] BSS"
        << bss
        << ": agents="
        << agents.size()
        << " availableStations="
        << config.nStationsPerAp
        << " maxAgentsPerStation="
        << (config.maxAgentsPerStation == 0
                ? std::string("unlimited")
                : std::to_string(config.maxAgentsPerStation))
        << " lowContentionPriority="
        << (config.lowContentionPriority ? "true" : "false"));

    // ========================================================================
    // Maximum-STA mode: choose mutually low-affinity seeds first.
    //
    // The old implementation took the most-conflicting agents and distributed
    // them across empty stations in round-robin order. That can split one dense
    // conflict cluster across several physical MAC contenders. Instead, choose
    // seeds that conflict as little as possible with already selected seeds.
    // ========================================================================

    std::unordered_set<int> seededAgents;

    if (!config.lowContentionPriority)
    {
        const int stationsToUse =
            std::min(
                config.nStationsPerAp,
                static_cast<int>(agents.size()));

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
            int64_t bestTotalAffinity =
                std::numeric_limits<int64_t>::max();

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
                    const int affinity =
                        getPairwiseAffinity(candidate, seed);

                    maxAffinityToSeed =
                        std::max(maxAffinityToSeed, affinity);

                    totalAffinityToSeeds += affinity;
                }

                // Primary: avoid choosing a seed strongly tied to any existing
                // seed. Secondary: minimize aggregate overlap with all seeds.
                // Remaining ties follow the already deterministic `agents`
                // order because we keep the first equivalent candidate.
                const bool isBetter =
                    bestSeed < 0 ||

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
                throw std::runtime_error(
                    "DistributeAgentsContentionAware: "
                    "failed to choose a STA seed agent");
            }

            seeds.push_back(bestSeed);
            seededAgents.insert(bestSeed);
        }

        // Exactly one seed per STA guarantees maximum possible STA utilization.
        for (int station = 0;
             station < static_cast<int>(seeds.size());
             ++station)
        {
            const int seed = seeds[station];

            commitStationAssignment(
                seed,
                station,
                static_cast<int>(activities[seed].uplinkSlots.size()),
                0,
                "seed");
        }
    }

    // ========================================================================
    // Greedy affinity-aware assignment.
    //
    // Primary objective:
    //   minimize newly active physical STA/time-slot pairs.
    //
    // Secondary objective:
    //   maximize pairwise overlap with agents already behind the same STA.
    //
    // Tertiary objectives:
    //   keep occupancy balanced, then use lower STA index deterministically.
    // ========================================================================

    for (int agentIndex : agents)
    {
        if (seededAgents.find(agentIndex) != seededAgents.end())
        {
            continue;
        }

        int bestStation = -1;

        int bestNewActiveSlots =
            std::numeric_limits<int>::max();

        int64_t bestStationAffinity =
            std::numeric_limits<int64_t>::min();

        for (int station = 0;
             station < config.nStationsPerAp;
             ++station)
        {
            // Hard placement constraint when configured.
            // maxAgentsPerStation == 0 means unlimited.
            if (config.maxAgentsPerStation > 0 &&
                agentCountPerStation[station] >=
                    config.maxAgentsPerStation)
            {
                continue;
            }

            const int newActiveSlots =
                calculateNewActiveSlots(agentIndex, station);

            const int64_t stationAffinity =
                calculateStationAffinity(agentIndex, station);

            const bool isBetter =
                bestStation < 0 ||

                newActiveSlots < bestNewActiveSlots ||

                (newActiveSlots == bestNewActiveSlots &&
                 stationAffinity > bestStationAffinity) ||

                (newActiveSlots == bestNewActiveSlots &&
                 stationAffinity == bestStationAffinity &&
                 agentCountPerStation[station] <
                     agentCountPerStation[bestStation]) ||

                (newActiveSlots == bestNewActiveSlots &&
                 stationAffinity == bestStationAffinity &&
                 agentCountPerStation[station] ==
                     agentCountPerStation[bestStation] &&
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
            throw std::runtime_error(
                "DistributeAgentsContentionAware: "
                "no STA with free agent capacity");
        }

        commitStationAssignment(
            agentIndex,
            bestStation,
            bestNewActiveSlots,
            bestStationAffinity,
            "affinity");
    }

    // ========================================================================
    // Final STA-level contention diagnostics.
    //
    // These are the most useful metrics for comparing the old algorithm with
    // low-contention placement.
    //
    // maxActiveUlSta:
    //
    //     Maximum number of distinct physical STAs with UL activity in any
    //     single time slot.
    //
    // sumActiveUlStaSlots:
    //
    //     Sum over all slots of:
    //
    //         number of distinct UL-active STAs
    //
    //     This is the main aggregate contention-oriented metric.
    // ========================================================================

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
        const int activeStationCount =
            static_cast<int>(stations.size());

        maxActiveUlSta =
            std::max(
                maxActiveUlSta,
                activeStationCount);

        sumActiveUlStaSlots += activeStationCount;

        NS_LOG_DEBUG(
            "[STA contention] BSS"
            << bss
            << " slot="
            << slot
            << " activeUlSta="
            << activeStationCount);
    }

    NS_LOG_INFO(
        "[STA assignment] BSS"
        << bss
        << " summary: agents="
        << agents.size()
        << " stationsUsed="
        << stationsUsed
        << " maxActiveUlSta="
        << maxActiveUlSta
        << " sumActiveUlStaSlots="
        << sumActiveUlStaSlots);
}

} // namespace ns3::llm_detail
