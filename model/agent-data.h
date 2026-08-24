#ifndef AGENT_DATA_H
#define AGENT_DATA_H

#include "ns3/address.h"

#include <map>
#include <string>
#include <vector>

namespace ns3
{

/**
 * One bidirectional application operation from an input trace.
 */
struct Operation
{
    int downlinkBytes;    ///< Downlink application payload size in bytes.
    double startOffsetMs; ///< Uplink send offset in milliseconds.
    double endMs;         ///< Downlink send offset in milliseconds.
    int uplinkBytes;      ///< Uplink application payload size in bytes.
};

/**
 * Parsed operations belonging to one application-level agent.
 */
struct AgentInfo
{
    std::string key;                   ///< Composite agent identifier.
    int id;                            ///< Numeric agent identifier.
    int type;                          ///< Deterministic numeric type identifier.
    std::vector<Operation> operations; ///< Network operations in trace order.
};

/**
 * Complete result of parsing an input trace.
 */
struct ParsedResult
{
    std::vector<AgentInfo> agents; ///< Parsed agents ordered by key.

    /**
     * Maximum operation end time in milliseconds.
     *
     * Local operations that do not generate network traffic still contribute
     * to the duration.
     */
    double experimentDurationMs{0.0};
};

/**
 * Agent placement and addressing produced by a distribution algorithm.
 */
struct DistributionResult
{
    std::vector<std::map<std::string, std::vector<Operation>>> apAgentMaps; ///< Agents by AP.
    std::vector<std::map<std::string, Address>> apStationMaps; ///< Station address by agent.
    std::vector<Address> apAddresses;                          ///< AP socket addresses.
    std::vector<Address> stationBases;                         ///< Station subnet base addresses.
};

} // namespace ns3

#endif // AGENT_DATA_H
