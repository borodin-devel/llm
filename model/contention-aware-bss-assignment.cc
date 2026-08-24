#include "contention-aware-distribution-internal.h"

#include "llm-log.h"

#include <algorithm>
#include <limits>
#include <queue>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ns3::llm_detail
{

static LogComponent& g_log = GetContentionAwareDistributionLog();

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
        for (int slot : activities[agentIndex].uplinkSlots)
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

            for (int slot : activities[selectedAgent].uplinkSlots)
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

        for (int slot : activities[selectedAgent].uplinkSlots)
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

        for (int slot : activities[selectedAgent].uplinkSlots)
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

} // namespace ns3::llm_detail
