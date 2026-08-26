#include "../logging/llm-log.h"
#include "contention-aware-distribution-internal.h"

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
 * Repeatedly selects the unassigned agent with the most remaining conflicts
 * and places it in the BSS with the smallest incremental pairwise UL overlap.
 * Ties prefer lower assigned bytes, fewer assigned agents, then lower BSS ID.
 */
std::vector<int>
AssignAgentsToBss(const std::vector<AgentActivity>& activities,
                  const ContentionAwareDistributionConfig& config)
{
    const int agentCount = static_cast<int>(activities.size());

    const bool hasAgentCapacityLimit = config.maxAgentsPerStation > 0;

    const int64_t agentsPerBssCapacity = hasAgentCapacityLimit
                                             ? static_cast<int64_t>(config.nStationsPerAp) *
                                                   static_cast<int64_t>(config.maxAgentsPerStation)
                                             : std::numeric_limits<int64_t>::max();

    // The inverse slot index makes conflict updates local to overlapping agents.
    std::unordered_map<int, std::vector<int>> agentsByUlSlot;

    for (int agentIndex = 0; agentIndex < agentCount; ++agentIndex)
    {
        for (int slot : activities[agentIndex].uplinkSlots)
        {
            agentsByUlSlot[slot].push_back(agentIndex);
        }
    }

    // Every shared slot contributes one conflict point per agent pair.
    std::vector<int64_t> remainingConflictScore(agentCount, 0);

    for (const auto& [slot, agents] : agentsByUlSlot)
    {
        (void)slot;

        const int64_t conflictContribution = static_cast<int64_t>(agents.size()) - 1;

        for (int agentIndex : agents)
        {
            remainingConflictScore[agentIndex] += conflictContribution;
        }
    }

    // Lazy heap entries are {score, -index}; stale scores are discarded on pop.
    using HeapEntry = std::pair<int64_t, int>;

    std::priority_queue<HeapEntry> conflictQueue;

    for (int agentIndex = 0; agentIndex < agentCount; ++agentIndex)
    {
        conflictQueue.push({remainingConflictScore[agentIndex], -agentIndex});
    }

    std::vector<bool> isRemaining(agentCount, true);
    std::vector<int> bssAssignment(agentCount, -1);

    // These are agent-level counts; physical STA placement happens later.
    std::vector<std::unordered_map<int, int>> ulSlotAgentCount(config.nAp);

    std::vector<int64_t> totalBytesPerBss(config.nAp, 0);
    std::vector<int> agentCountPerBss(config.nAp, 0);

    int remainingAgents = agentCount;

    while (remainingAgents > 0)
    {
        int selectedAgent = -1;

        while (!conflictQueue.empty())
        {
            const auto [storedScore, negativeAgentIndex] = conflictQueue.top();

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
            throw std::runtime_error("DistributeAgentsContentionAware: "
                                     "failed to select an unassigned agent");
        }

        int bestBss = -1;

        int64_t bestOverlapCost = std::numeric_limits<int64_t>::max();

        for (int bss = 0; bss < config.nAp; ++bss)
        {
            // A positive maxAgentsPerStation imposes a hard BSS capacity.
            // Zero means unlimited application-level agents per physical STA.
            if (hasAgentCapacityLimit &&
                static_cast<int64_t>(agentCountPerBss[bss]) >= agentsPerBssCapacity)
            {
                continue;
            }

            int64_t overlapCost = 0;

            for (int slot : activities[selectedAgent].uplinkSlots)
            {
                const auto existing = ulSlotAgentCount[bss].find(slot);

                if (existing != ulSlotAgentCount[bss].end())
                {
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
                 totalBytesPerBss[bss] < totalBytesPerBss[bestBss]) ||

                (overlapCost == bestOverlapCost &&
                 totalBytesPerBss[bss] == totalBytesPerBss[bestBss] &&
                 agentCountPerBss[bss] < agentCountPerBss[bestBss]) ||

                (overlapCost == bestOverlapCost &&
                 totalBytesPerBss[bss] == totalBytesPerBss[bestBss] &&
                 agentCountPerBss[bss] == agentCountPerBss[bestBss] && bss < bestBss);

            if (isBetter)
            {
                bestBss = bss;
                bestOverlapCost = overlapCost;
            }
        }

        if (bestBss < 0)
        {
            throw std::runtime_error("DistributeAgentsContentionAware: "
                                     "all BSSes reached their agent capacity");
        }

        bssAssignment[selectedAgent] = bestBss;

        ++agentCountPerBss[bestBss];

        totalBytesPerBss[bestBss] += activities[selectedAgent].totalBytes;

        for (int slot : activities[selectedAgent].uplinkSlots)
        {
            ++ulSlotAgentCount[bestBss][slot];
        }

        NS_LOG_INFO("[BSS assignment] agent=\""
                    << activities[selectedAgent].agent->key
                    << "\" conflictScore=" << remainingConflictScore[selectedAgent] << " -> BSS"
                    << bestBss << " incrementalUlOverlap=" << bestOverlapCost << " bssAgents="
                    << agentCountPerBss[bestBss] << " bssBytes=" << totalBytesPerBss[bestBss]);

        // Removing an agent decrements each still-active neighbor once per shared slot.
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
                conflictQueue.push({remainingConflictScore[otherAgent], -otherAgent});
            }
        }
    }

    for (int bss = 0; bss < config.nAp; ++bss)
    {
        int maxConcurrentUlAgents = 0;
        int64_t sumConcurrentUlAgents = 0;

        for (const auto& [slot, count] : ulSlotAgentCount[bss])
        {
            (void)slot;

            maxConcurrentUlAgents = std::max(maxConcurrentUlAgents, count);

            sumConcurrentUlAgents += count;
        }

        NS_LOG_INFO("[BSS assignment] BSS" << bss << " summary: agents=" << agentCountPerBss[bss]
                                           << " totalBytes=" << totalBytesPerBss[bss]
                                           << " maxConcurrentUlAgents=" << maxConcurrentUlAgents
                                           << " sumUlAgentSlots=" << sumConcurrentUlAgents);
    }

    return bssAssignment;
}

} // namespace ns3::llm_detail
