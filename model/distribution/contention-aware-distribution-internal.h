#ifndef CONTENTION_AWARE_DISTRIBUTION_INTERNAL_H
#define CONTENTION_AWARE_DISTRIBUTION_INTERNAL_H

#include "contention-aware-agent-distribution.h"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace ns3::llm_detail
{

/**
 * Slot-oriented activity used by the contention-aware placement phases.
 */
struct AgentActivity
{
    const AgentInfo* agent{nullptr}; ///< Source agent owned by ParsedResult.
    std::set<int> uplinkSlots;       ///< Slots containing uplink starts.
    int64_t totalBytes{0};           ///< Downlink plus uplink bytes.
};

/**
 * Calculate the bidirectional payload total for one agent.
 *
 * @param operations Agent operations.
 * @return Downlink plus uplink bytes.
 */
int64_t CalculateTotalBytes(const std::vector<Operation>& operations);

/**
 * Convert agent traces into uplink activity slots.
 *
 * @param agents Parsed agents.
 * @param slotMs Activity-slot width in milliseconds.
 * @return Slot-oriented agent activity.
 */
std::vector<AgentActivity> BuildAgentActivities(const std::vector<AgentInfo>& agents, int slotMs);

/**
 * Assign every activity to a BSS.
 *
 * @param activities Slot-oriented agent activity.
 * @param config Distribution configuration.
 * @return BSS index for every activity.
 */
std::vector<int> AssignAgentsToBss(const std::vector<AgentActivity>& activities,
                                   const ContentionAwareDistributionConfig& config);

/**
 * Assign activities in one BSS to physical stations.
 *
 * @param activities Slot-oriented agent activity.
 * @param bssAssignment BSS index for every activity.
 * @param bssIndex BSS to process.
 * @param config Distribution configuration.
 * @param stationAddressByAgent Output station address for each assigned agent.
 */
void AssignAgentsToStations(const std::vector<AgentActivity>& activities,
                            const std::vector<int>& bssAssignment,
                            int bssIndex,
                            const ContentionAwareDistributionConfig& config,
                            std::map<std::string, Address>& stationAddressByAgent);

} // namespace ns3::llm_detail

#endif // CONTENTION_AWARE_DISTRIBUTION_INTERNAL_H
