#include "contention-aware-agent-distribution.h"

#include "../logging/llm-log.h"
#include "contention-aware-distribution-internal.h"

#include "ns3/inet-socket-address.h"
#include "ns3/ipv4-address.h"

#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ns3
{

static LogComponent& g_log = llm_detail::GetContentionAwareDistributionLog();

namespace llm_detail
{

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
    std::vector<AgentActivity> activities;
    activities.reserve(agents.size());

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

            activity.uplinkSlots.insert(static_cast<int>(
                std::floor(operation.startOffsetMs / static_cast<double>(slotMs))));
        }

        activities.push_back(std::move(activity));
    }

    return activities;
}

} // namespace llm_detail

using llm_detail::AgentActivity;
using llm_detail::AssignAgentsToBss;
using llm_detail::AssignAgentsToStations;
using llm_detail::BuildAgentActivities;

namespace
{

void
ValidateConfiguration(const ParsedResult& parsedTrace,
                      const ContentionAwareDistributionConfig& config)
{
    if (config.nAp <= 0)
    {
        throw std::invalid_argument("DistributeAgentsContentionAware: nAp must be > 0");
    }

    if (config.nStationsPerAp <= 0)
    {
        throw std::invalid_argument("DistributeAgentsContentionAware: "
                                    "nStationsPerAp must be > 0");
    }

    if (config.maxAgentsPerStation < 0)
    {
        throw std::invalid_argument("DistributeAgentsContentionAware: "
                                    "maxAgentsPerStation must be >= 0 (0 means unlimited)");
    }

    if (config.slotMs <= 0)
    {
        throw std::invalid_argument("DistributeAgentsContentionAware: slotMs must be > 0");
    }

    if (config.maxAgentsPerStation > 0)
    {
        const int64_t totalCapacity = static_cast<int64_t>(config.nAp) *
                                      static_cast<int64_t>(config.nStationsPerAp) *
                                      static_cast<int64_t>(config.maxAgentsPerStation);

        if (static_cast<int64_t>(parsedTrace.agents.size()) > totalCapacity)
        {
            throw std::invalid_argument("DistributeAgentsContentionAware: "
                                        "agent count exceeds total BSS/STA capacity");
        }
    }
}

} // namespace

DistributionResult
DistributeAgentsContentionAware(const ParsedResult& parsedTrace,
                                const ContentionAwareDistributionConfig& config)
{
    ValidateConfiguration(parsedTrace, config);

    NS_LOG_INFO("[Distribution] Starting contention-aware distribution:"
                << " agents=" << parsedTrace.agents.size() << " BSS=" << config.nAp
                << " stationsPerBss=" << config.nStationsPerAp << " maxAgentsPerStation="
                << (config.maxAgentsPerStation == 0 ? std::string("unlimited")
                                                    : std::to_string(config.maxAgentsPerStation))
                << " lowContentionPriority=" << (config.lowContentionPriority ? "true" : "false")
                << " slotMs=" << config.slotMs
                << " experimentDurationMs=" << parsedTrace.experimentDurationMs);

    DistributionResult distribution;

    distribution.apAgentMaps.resize(config.nAp);
    distribution.apStationMaps.resize(config.nAp);
    distribution.apAddresses.resize(config.nAp);
    distribution.stationBases.resize(config.nAp);

    // SetupApGroup expects the AP to own 10.1.<bss>.1.
    for (int bssIndex = 0; bssIndex < config.nAp; ++bssIndex)
    {
        const std::string accessPointIp = "10.1." + std::to_string(bssIndex) + ".1";

        distribution.apAddresses[bssIndex] =
            InetSocketAddress(Ipv4Address(accessPointIp.c_str()), 10000);

        distribution.stationBases[bssIndex] =
            InetSocketAddress(Ipv4Address(accessPointIp.c_str()), 9000);
    }

    const std::vector<AgentActivity> activities =
        BuildAgentActivities(parsedTrace.agents, config.slotMs);

    const std::vector<int> bssAssignment = AssignAgentsToBss(activities, config);

    // Copy each agent's original operations into the corresponding BSS map.
    //
    // The traffic generators consume these operations later, therefore the
    // distribution algorithm must not modify their timestamps or byte counts.
    for (int agentIndex = 0; agentIndex < static_cast<int>(activities.size()); ++agentIndex)
    {
        const int bssIndex = bssAssignment[agentIndex];

        if (bssIndex < 0 || bssIndex >= config.nAp)
        {
            throw std::runtime_error("DistributeAgentsContentionAware: "
                                     "invalid BSS assignment");
        }

        distribution.apAgentMaps[bssIndex][activities[agentIndex].agent->key] =
            activities[agentIndex].agent->operations;
    }

    for (int bssIndex = 0; bssIndex < config.nAp; ++bssIndex)
    {
        AssignAgentsToStations(activities,
                               bssAssignment,
                               bssIndex,
                               config,
                               distribution.apStationMaps[bssIndex]);
    }

    std::size_t mappedAgentCount = 0;

    for (const auto& stationMap : distribution.apStationMaps)
    {
        mappedAgentCount += stationMap.size();
    }

    if (mappedAgentCount != parsedTrace.agents.size())
    {
        throw std::runtime_error("DistributeAgentsContentionAware: "
                                 "not all agents were assigned to a station");
    }

    NS_LOG_INFO("[Distribution] Contention-aware distribution complete:"
                << " mappedAgents=" << mappedAgentCount << "/" << parsedTrace.agents.size());

    return distribution;
}
} // namespace ns3
