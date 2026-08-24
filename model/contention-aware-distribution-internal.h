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

int64_t CalculateTotalBytes(const std::vector<Operation>& operations);

std::vector<AgentActivity> BuildAgentActivities(const std::vector<AgentInfo>& agents, int slotMs);

std::vector<int> AssignAgentsToBss(const std::vector<AgentActivity>& activities,
                                   const ContentionAwareDistributionConfig& config);

void AssignAgentsToStations(const std::vector<AgentActivity>& activities,
                            const std::vector<int>& bssAssignment,
                            int bssIndex,
                            const ContentionAwareDistributionConfig& config,
                            std::map<std::string, Address>& stationAddressByAgent);

} // namespace ns3::llm_detail

#endif // CONTENTION_AWARE_DISTRIBUTION_INTERNAL_H
