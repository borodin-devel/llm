#ifndef SCENARIO_TOPOLOGY_H
#define SCENARIO_TOPOLOGY_H

#include "ns3/address.h"
#include "ns3/agent-data.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ns3
{

class TrafficCoordinator;
class WifiStatistics;

/**
 * Create one isolated AP group and install its traffic applications.
 *
 * @param bssIndex Zero-based AP-group index.
 * @param bandwidthMhz Wi-Fi channel width in MHz.
 * @param stationAddressByAgent Assigned station address for each agent.
 * @param operationsByAgent Traffic operations for each agent.
 * @param apAddress AP address used by station generators.
 * @param stationCount Number of physical stations to create.
 * @param coordinator Traffic readiness and lifetime coordinator.
 * @param statistics Wi-Fi statistics owner.
 */
void SetupApGroup(int bssIndex,
                  int bandwidthMhz,
                  const std::map<std::string, Address>& stationAddressByAgent,
                  const std::map<std::string, std::vector<Operation>>& operationsByAgent,
                  Address apAddress,
                  uint32_t stationCount,
                  TrafficCoordinator& coordinator,
                  WifiStatistics& statistics);

} // namespace ns3

#endif // SCENARIO_TOPOLOGY_H
