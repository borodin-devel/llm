#include "contention-aware-agent-distribution.h"

#include "ns3/inet-socket-address.h"
#include "ns3/ipv4-address.h"
#include "ns3/log.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <queue>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("ContentionAwareAgentDistribution");

namespace
{

// ============================================================================
// Internal representation
// ============================================================================

/**
 * Compact representation used by the distribution algorithm.
 *
 * We intentionally keep only UL-active slots here.
 *
 * Why UL?
 *
 * Downlink transmissions originate from the AP. Multiple DL flows therefore
 * do not create multiple independent Wi-Fi contenders: they are serialized
 * through the AP/MAC.
 *
 * Uplink transmissions are different. Every active STA is an independent
 * contender for the shared medium. Therefore the number of simultaneously
 * UL-active STAs is the primary quantity we are trying to reduce.
 *
 * totalBytes contains DL + UL traffic and is used only as a BSS tie-breaker.
 * It is NOT treated as the primary contention metric.
 */
struct AgentActivity
{
    const AgentInfo* agent{nullptr};

    /**
     * UL slots in which this agent becomes active.
     *
     * std::set is used intentionally: if an agent has multiple operations
     * whose startOffsetMs values fall into the same slot, that agent still counts as
     * one active application-level source for conflict calculations.
     */
    std::set<int> ulSlots;

    /// Total DL + UL bytes for secondary BSS load balancing.
    int64_t totalBytes{0};
};


// ============================================================================
// Generic helpers
// ============================================================================

/**
 * Calculate total application payload associated with one agent.
 *
 * The value is deliberately not used as the main contention cost. It serves
 * only as a deterministic tie-breaker when several BSS choices have the same
 * UL-overlap cost.
 */
int64_t
CalculateTotalBytes(const std::vector<Operation>& operations)
{
    int64_t total = 0;

    for (const auto& operation : operations)
    {
        total += static_cast<int64_t>(operation.downlinkBytes);
        total += static_cast<int64_t>(operation.uplinkBytes);
    }

    return total;
}


/**
 * Convert ParsedResult into the slot-oriented representation used below.
 *
 * An UL transmission is represented by the slot containing operation.startOffsetMs:
 *
 *     slot = startOffsetMs / slotMs
 *
 * We do not attempt to model UL transmission duration here because the input
 * trace only gives us the UL start timestamp and payload size, not an actual
 * PHY/MAC airtime interval.
 *
 * This keeps the heuristic directly tied to information that exists in the
 * trace instead of inventing a transmission duration model.
 */
std::vector<AgentActivity>
BuildAgentActivities(const std::vector<AgentInfo>& agents, int slotMs)
{
    std::vector<AgentActivity> result;
    result.reserve(agents.size());

    for (const auto& agent : agents)
    {
        AgentActivity activity;

        activity.agent = &agent;
        activity.totalBytes = CalculateTotalBytes(agent.operations);

        for (const auto& operation : agent.operations)
        {
            // Negative timestamps should not normally exist in the trace.
            // Ignore them defensively rather than producing a negative slot.
            if (operation.startOffsetMs < 0)
            {
                continue;
            }

            activity.ulSlots.insert(static_cast<int>(
                std::floor(operation.startOffsetMs /
                           static_cast<double>(slotMs))));
        }

        result.push_back(std::move(activity));
    }

    return result;
}


// ============================================================================
// BSS assignment
// ============================================================================

/**
 * Assign agents to BSSes.
 *
 * Primary objective:
 *
 *     minimize pairwise UL overlap between agents inside the same BSS.
 *
 * Example:
 *
 *     slot 10:
 *       Agent A
 *       Agent B
 *       Agent C
 *
 * Prefer:
 *
 *     A -> BSS0
 *     B -> BSS1
 *     C -> BSS2
 *
 * instead of putting all three into one BSS.
 *
 *
 * Agent selection
 * ---------------
 *
 * The algorithm is dynamic rather than a one-time static sort.
 *
 * At every iteration we choose the still-unassigned agent that currently has
 * the largest number of conflicts with other still-unassigned agents.
 *
 * After assigning it, the conflict scores of its remaining neighbors are
 * reduced. This approximates the "recompute concurrency after every choice"
 * behavior described in the design without requiring an expensive complete
 * graph reconstruction on every iteration.
 *
 *
 * BSS selection
 * -------------
 *
 * For the selected agent we evaluate every BSS.
 *
 * The primary cost is incremental pairwise UL overlap:
 *
 *     if the candidate agent uses slot S and a BSS already contains N agents
 *     active in S, adding the candidate creates N new overlap pairs.
 *
 * Total overlap cost is summed across all UL slots of the candidate.
 *
 * Tie-breaks are:
 *
 *   1. lower total DL+UL bytes already assigned to BSS;
 *   2. lower number of agents already assigned;
 *   3. lower BSS index for deterministic output.
 */
std::vector<int>
AssignAgentsToBss(
    const std::vector<AgentActivity>& activities,
    const ContentionAwareDistributionConfig& config)
{
    const int agentCount = static_cast<int>(activities.size());

    const bool hasAgentCapacityLimit =
        config.maxAgentsPerStation > 0;

    const int64_t agentsPerBssCapacity =
        hasAgentCapacityLimit
            ? static_cast<int64_t>(config.nStationsPerAp) *
                  static_cast<int64_t>(config.maxAgentsPerStation)
            : std::numeric_limits<int64_t>::max();

    // ------------------------------------------------------------------------
    // Build inverse slot index:
    //
    //     UL slot -> indices of agents active in that slot.
    //
    // This allows conflict scores to be calculated and updated efficiently.
    // ------------------------------------------------------------------------

    std::unordered_map<int, std::vector<int>> agentsByUlSlot;

    for (int agentIndex = 0; agentIndex < agentCount; ++agentIndex)
    {
        for (int slot : activities[agentIndex].ulSlots)
        {
            agentsByUlSlot[slot].push_back(agentIndex);
        }
    }

    // ------------------------------------------------------------------------
    // Initial conflict score.
    //
    // If a slot contains N agents, every agent in that slot conflicts with
    // N - 1 other agents.
    //
    // If the same pair overlaps in several slots, every shared slot adds one
    // point. That is intentional: repeatedly overlapping agents are more
    // important to separate than agents which meet only once.
    // ------------------------------------------------------------------------

    std::vector<int64_t> remainingConflictScore(agentCount, 0);

    for (const auto& [slot, agents] : agentsByUlSlot)
    {
        (void)slot;

        const int64_t conflictContribution =
            static_cast<int64_t>(agents.size()) - 1;

        for (int agentIndex : agents)
        {
            remainingConflictScore[agentIndex] += conflictContribution;
        }
    }

    // ------------------------------------------------------------------------
    // Lazy max-heap used to select the currently most conflicting agent.
    //
    // Scores change when agents are removed from the remaining set. Instead
    // of editing heap entries in place, we push a new entry every time a score
    // changes. Old entries remain in the heap and are discarded when popped.
    //
    // Pair layout:
    //
    //     { conflictScore, -agentIndex }
    //
    // The negative index gives deterministic behavior for equal scores:
    // lower original agent index wins.
    // ------------------------------------------------------------------------

    using HeapEntry = std::pair<int64_t, int>;

    std::priority_queue<HeapEntry> conflictQueue;

    for (int agentIndex = 0; agentIndex < agentCount; ++agentIndex)
    {
        conflictQueue.push(
            {remainingConflictScore[agentIndex], -agentIndex});
    }

    std::vector<bool> isRemaining(agentCount, true);
    std::vector<int> bssAssignment(agentCount, -1);

    // ------------------------------------------------------------------------
    // Current BSS state.
    //
    // For every BSS we track:
    //
    //   ulSlotAgentCount[bss][slot]
    //
    // which tells us how many already-assigned agents have an UL operation
    // in a given slot.
    //
    // Note that this is an AGENT-level metric. STA mapping happens later.
    // ------------------------------------------------------------------------

    std::vector<std::unordered_map<int, int>>
        ulSlotAgentCount(config.nAp);

    std::vector<int64_t> totalBytesPerBss(config.nAp, 0);
    std::vector<int> agentCountPerBss(config.nAp, 0);

    int remainingAgents = agentCount;

    while (remainingAgents > 0)
    {
        // ====================================================================
        // Select the currently most conflicting unassigned agent.
        // ====================================================================

        int selectedAgent = -1;

        while (!conflictQueue.empty())
        {
            const auto [storedScore, negativeAgentIndex] =
                conflictQueue.top();

            conflictQueue.pop();

            const int agentIndex = -negativeAgentIndex;

            // Agent was already assigned after this heap entry was created.
            if (!isRemaining[agentIndex])
            {
                continue;
            }

            // Score changed after this heap entry was created.
            if (storedScore != remainingConflictScore[agentIndex])
            {
                continue;
            }

            selectedAgent = agentIndex;
            break;
        }

        if (selectedAgent < 0)
        {
            throw std::runtime_error(
                "DistributeAgentsContentionAware: "
                "failed to select an unassigned agent");
        }

        // ====================================================================
        // Evaluate all BSS candidates.
        // ====================================================================

        int bestBss = -1;

        int64_t bestOverlapCost =
            std::numeric_limits<int64_t>::max();

        for (int bss = 0; bss < config.nAp; ++bss)
        {
            // A positive maxAgentsPerStation imposes a hard BSS capacity.
            // Zero means unlimited application-level agents per physical STA.
            if (hasAgentCapacityLimit &&
                static_cast<int64_t>(agentCountPerBss[bss]) >=
                    agentsPerBssCapacity)
            {
                continue;
            }

            int64_t overlapCost = 0;

            for (int slot : activities[selectedAgent].ulSlots)
            {
                const auto existing =
                    ulSlotAgentCount[bss].find(slot);

                if (existing != ulSlotAgentCount[bss].end())
                {
                    // Example:
                    //
                    // already 3 agents active in this slot
                    // + candidate
                    //
                    // => candidate creates 3 new pairwise overlaps.
                    overlapCost += existing->second;
                }
            }

            // Primary objective: smallest UL overlap.
            //
            // Bytes and number of assigned agents are used only if several
            // BSSes are equivalent according to the primary objective.
            const bool isBetter =
                bestBss < 0 ||

                overlapCost < bestOverlapCost ||

                (overlapCost == bestOverlapCost &&
                 totalBytesPerBss[bss] <
                     totalBytesPerBss[bestBss]) ||

                (overlapCost == bestOverlapCost &&
                 totalBytesPerBss[bss] ==
                     totalBytesPerBss[bestBss] &&
                 agentCountPerBss[bss] <
                     agentCountPerBss[bestBss]) ||

                (overlapCost == bestOverlapCost &&
                 totalBytesPerBss[bss] ==
                     totalBytesPerBss[bestBss] &&
                 agentCountPerBss[bss] ==
                     agentCountPerBss[bestBss] &&
                 bss < bestBss);

            if (isBetter)
            {
                bestBss = bss;
                bestOverlapCost = overlapCost;
            }
        }

        if (bestBss < 0)
        {
            throw std::runtime_error(
                "DistributeAgentsContentionAware: "
                "all BSSes reached their agent capacity");
        }

        // ====================================================================
        // Commit BSS assignment.
        // ====================================================================

        bssAssignment[selectedAgent] = bestBss;

        ++agentCountPerBss[bestBss];

        totalBytesPerBss[bestBss] +=
            activities[selectedAgent].totalBytes;

        for (int slot : activities[selectedAgent].ulSlots)
        {
            ++ulSlotAgentCount[bestBss][slot];
        }

        NS_LOG_INFO(
            "[BSS assignment] agent=\""
            << activities[selectedAgent].agent->key
            << "\" conflictScore="
            << remainingConflictScore[selectedAgent]
            << " -> BSS" << bestBss
            << " incrementalUlOverlap="
            << bestOverlapCost
            << " bssAgents="
            << agentCountPerBss[bestBss]
            << " bssBytes="
            << totalBytesPerBss[bestBss]);

        // ====================================================================
        // Remove the selected agent from the remaining conflict graph.
        //
        // Suppose:
        //
        //     slot 5 = {A, B, C}
        //
        // Initially:
        //
        //     score(A) += 2
        //     score(B) += 2
        //     score(C) += 2
        //
        // After A is assigned, B and C each have one fewer remaining conflict
        // in this slot, so both scores are decremented by one.
        // ====================================================================

        isRemaining[selectedAgent] = false;
        --remainingAgents;

        for (int slot : activities[selectedAgent].ulSlots)
        {
            const auto slotEntry = agentsByUlSlot.find(slot);

            if (slotEntry == agentsByUlSlot.end())
            {
                continue;
            }

            for (int otherAgent : slotEntry->second)
            {
                if (!isRemaining[otherAgent])
                {
                    continue;
                }

                --remainingConflictScore[otherAgent];

                // Lazy heap update. The old score remains in the heap but
                // will be recognized as stale and discarded later.
                conflictQueue.push(
                    {remainingConflictScore[otherAgent], -otherAgent});
            }
        }
    }

    // ========================================================================
    // BSS diagnostics
    // ========================================================================

    for (int bss = 0; bss < config.nAp; ++bss)
    {
        int maxConcurrentUlAgents = 0;
        int64_t sumConcurrentUlAgents = 0;

        for (const auto& [slot, count] : ulSlotAgentCount[bss])
        {
            (void)slot;

            maxConcurrentUlAgents =
                std::max(maxConcurrentUlAgents, count);

            sumConcurrentUlAgents += count;
        }

        NS_LOG_INFO(
            "[BSS assignment] BSS"
            << bss
            << " summary: agents="
            << agentCountPerBss[bss]
            << " totalBytes="
            << totalBytesPerBss[bss]
            << " maxConcurrentUlAgents="
            << maxConcurrentUlAgents
            << " sumUlAgentSlots="
            << sumConcurrentUlAgents);
    }

    return bssAssignment;
}


// ============================================================================
// STA assignment
// ============================================================================

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
        for (int slot : activities[agentIndex].ulSlots)
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

            if (activities[lhs].ulSlots.size() !=
                activities[rhs].ulSlots.size())
            {
                return activities[lhs].ulSlots.size() >
                       activities[rhs].ulSlots.size();
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

            for (int slot : activities[agentIndex].ulSlots)
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

            for (int slot : activities[agentIndex].ulSlots)
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
                static_cast<int>(activities[seed].ulSlots.size()),
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

        for (int slot : activities[agentIndex].ulSlots)
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


// ============================================================================
// Configuration validation
// ============================================================================

void
ValidateConfiguration(
    const ParsedResult& parsed,
    const ContentionAwareDistributionConfig& config)
{
    if (config.nAp <= 0)
    {
        throw std::invalid_argument(
            "DistributeAgentsContentionAware: nAp must be > 0");
    }

    if (config.nStationsPerAp <= 0)
    {
        throw std::invalid_argument(
            "DistributeAgentsContentionAware: "
            "nStationsPerAp must be > 0");
    }

    if (config.maxAgentsPerStation < 0)
    {
        throw std::invalid_argument(
            "DistributeAgentsContentionAware: "
            "maxAgentsPerStation must be >= 0 (0 means unlimited)");
    }

    if (config.slotMs <= 0)
    {
        throw std::invalid_argument(
            "DistributeAgentsContentionAware: slotMs must be > 0");
    }

    if (config.maxAgentsPerStation > 0)
    {
        const int64_t totalCapacity =
            static_cast<int64_t>(config.nAp) *
            static_cast<int64_t>(config.nStationsPerAp) *
            static_cast<int64_t>(config.maxAgentsPerStation);

        if (static_cast<int64_t>(parsed.agents.size()) > totalCapacity)
        {
            throw std::invalid_argument(
                "DistributeAgentsContentionAware: "
                "agent count exceeds total BSS/STA capacity");
        }
    }
}

} // anonymous namespace


// ============================================================================
// Public API
// ============================================================================

DistributionResult
DistributeAgentsContentionAware(
    const ParsedResult& parsed,
    const ContentionAwareDistributionConfig& config)
{
    ValidateConfiguration(parsed, config);

    NS_LOG_INFO(
        "[Distribution] Starting contention-aware distribution:"
        << " agents=" << parsed.agents.size()
        << " BSS=" << config.nAp
        << " stationsPerBss=" << config.nStationsPerAp
        << " maxAgentsPerStation="
        << (config.maxAgentsPerStation == 0
                ? std::string("unlimited")
                : std::to_string(config.maxAgentsPerStation))
        << " lowContentionPriority="
        << (config.lowContentionPriority ? "true" : "false")
        << " slotMs=" << config.slotMs
        << " experimentDurationMs=" << parsed.experimentDurationMs);

    DistributionResult result;

    result.apAgentMaps.resize(config.nAp);
    result.apStationMaps.resize(config.nAp);
    result.apAddresses.resize(config.nAp);
    result.stationBases.resize(config.nAp);

    // ------------------------------------------------------------------------
    // Preserve the exact addressing scheme expected by SetupApGroup().
    // ------------------------------------------------------------------------

    for (int bss = 0; bss < config.nAp; ++bss)
    {
        const std::string apIp =
            "10.1." +
            std::to_string(bss) +
            ".1";

        result.apAddresses[bss] =
            InetSocketAddress(
                Ipv4Address(apIp.c_str()),
                10000);

        result.stationBases[bss] =
            InetSocketAddress(
                Ipv4Address(apIp.c_str()),
                9000);
    }

    // ------------------------------------------------------------------------
    // Convert trace operations into UL time-slot activity.
    // ------------------------------------------------------------------------

    const std::vector<AgentActivity> activities =
        BuildAgentActivities(
            parsed.agents,
            config.slotMs);

    // ------------------------------------------------------------------------
    // Phase 1:
    // distribute application-level agents between independent BSSes.
    // ------------------------------------------------------------------------

    const std::vector<int> bssAssignment =
        AssignAgentsToBss(
            activities,
            config);

    // Copy each agent's original operations into the corresponding BSS map.
    //
    // The traffic generators consume these operations later, therefore the
    // distribution algorithm must not modify their timestamps or byte counts.
    for (int agentIndex = 0;
         agentIndex < static_cast<int>(activities.size());
         ++agentIndex)
    {
        const int bss = bssAssignment[agentIndex];

        if (bss < 0 || bss >= config.nAp)
        {
            throw std::runtime_error(
                "DistributeAgentsContentionAware: "
                "invalid BSS assignment");
        }

        result.apAgentMaps[bss]
                          [activities[agentIndex].agent->key] =
            activities[agentIndex].agent->operations;
    }

    // ------------------------------------------------------------------------
    // Phase 2:
    // independently map agents to physical STAs inside every BSS.
    // ------------------------------------------------------------------------

    for (int bss = 0; bss < config.nAp; ++bss)
    {
        AssignAgentsToStations(
            activities,
            bssAssignment,
            bss,
            config,
            result.apStationMaps[bss]);
    }

    // ------------------------------------------------------------------------
    // Sanity check.
    //
    // The old implementation can silently skip an agent if all stations are
    // full. This implementation treats incomplete placement as an error.
    // ------------------------------------------------------------------------

    std::size_t mappedAgents = 0;

    for (const auto& stationMap : result.apStationMaps)
    {
        mappedAgents += stationMap.size();
    }

    if (mappedAgents != parsed.agents.size())
    {
        throw std::runtime_error(
            "DistributeAgentsContentionAware: "
            "not all agents were assigned to a station");
    }

    NS_LOG_INFO(
        "[Distribution] Contention-aware distribution complete:"
        << " mappedAgents="
        << mappedAgents
        << "/"
        << parsed.agents.size());

    return result;
}

} // namespace ns3