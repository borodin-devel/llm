#ifndef SCENARIO_TOPOLOGY_H
#define SCENARIO_TOPOLOGY_H

#include "../config/scenario-config.h"

#include "ns3/address.h"
#include "ns3/agent-data.h"
#include "ns3/yans-wifi-channel.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ns3
{

class TrafficCoordinator;
class ExperimentStatistics;

/**
 * Create a Yans channel with the default propagation delay and loss models.
 *
 * @return Newly created Yans channel.
 */
Ptr<YansWifiChannel> CreateDefaultYansChannel();

/**
 * Select the channel for one BSS.
 *
 * @param isolateBssChannels Whether the BSS must use an isolated channel.
 * @param sharedChannel Channel shared by non-isolated BSS groups.
 * @return A new default channel when isolated, otherwise the shared channel.
 */
Ptr<YansWifiChannel> SelectBssChannel(bool isolateBssChannels, Ptr<YansWifiChannel> sharedChannel);

/**
 * Build the ns-3 ChannelSettings tuple for a Wi-Fi configuration.
 *
 * @param wifiConfig Validated Wi-Fi configuration.
 * @return ChannelSettings attribute value.
 */
std::string BuildChannelSettings(const WifiConfig& wifiConfig);

/**
 * Create one AP group and install its traffic applications.
 *
 * @param bssIndex Zero-based AP-group index.
 * @param topologyConfig Validated topology configuration.
 * @param wifiConfig Validated Wi-Fi configuration.
 * @param sharedChannel Channel shared by non-isolated BSS groups.
 * @param stationAddressByAgent Assigned station address for each agent.
 * @param operationsByAgent Traffic operations for each agent.
 * @param accessPointAddress AP address used by station generators.
 * @param coordinator Traffic readiness and lifetime coordinator.
 * @param statistics Experiment statistics owner.
 */
void SetupApGroup(int bssIndex,
                  const TopologyConfig& topologyConfig,
                  const WifiConfig& wifiConfig,
                  Ptr<YansWifiChannel> sharedChannel,
                  const std::map<std::string, Address>& stationAddressByAgent,
                  const std::map<std::string, std::vector<Operation>>& operationsByAgent,
                  Address accessPointAddress,
                  TrafficCoordinator& coordinator,
                  ExperimentStatistics& statistics);

} // namespace ns3

#endif // SCENARIO_TOPOLOGY_H
