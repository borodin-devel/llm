#ifndef AGENT_DISTRIBUTION_H
#define AGENT_DISTRIBUTION_H

#include "agent-data.h"
#include "app-tx-tag.h"
#include "trace-parser.h"

namespace ns3
{

/**
 * Distribute agents across APs and stations with the original greedy heuristic.
 *
 * @param parsedTrace Parsed agent trace.
 * @param accessPointCount Number of APs.
 * @param stationsPerAccessPoint Number of stations available per AP.
 * @param maxAgentsPerStation Maximum number of agents assigned to one station.
 * @return AP and station placement for every assigned agent.
 */
DistributionResult DistributeAgents(const ParsedResult& parsedTrace,
                                    int accessPointCount = 3,
                                    int stationsPerAccessPoint = 30,
                                    int maxAgentsPerStation = 3);

} // namespace ns3

#endif // AGENT_DISTRIBUTION_H
