#ifndef TRAFFIC_SCHEDULE_H
#define TRAFFIC_SCHEDULE_H

#include "ns3/address.h"
#include "ns3/nstime.h"

#include <cstdint>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace ns3
{

/** Existing public operation representation used by both generators. */
using LegacyAgentOperations =
    std::map<std::string, std::vector<std::tuple<int, double, double, int>>>;

/** One payload scheduled against the common trace epoch. */
struct ScheduledPayload
{
    std::string agentKey;     ///< Application-level agent identifier.
    uint32_t payloadBytes{0}; ///< Application payload size in bytes.
    double traceTimeMs{0.0};  ///< Offset from the common epoch in milliseconds.
};

/** Downlink payload schedules grouped by destination station. */
using DownlinkSchedulesByStation = std::map<Address, std::vector<ScheduledPayload>>;

/**
 * Build an uplink schedule ordered by operation start offset.
 *
 * @param operationsByAgent Legacy operations grouped by agent.
 * @return Ordered uplink payload schedule.
 */
std::vector<ScheduledPayload> BuildUplinkSchedule(
    const LegacyAgentOperations& operationsByAgent);

/**
 * Build downlink schedules ordered by operation end offset.
 *
 * Agents without a station address are omitted.
 *
 * @param operationsByAgent Legacy operations grouped by agent.
 * @param stationAddressByAgent Destination station address by agent.
 * @return Ordered downlink schedules grouped by station.
 */
DownlinkSchedulesByStation BuildDownlinkSchedules(
    const LegacyAgentOperations& operationsByAgent,
    const std::map<std::string, Address>& stationAddressByAgent);

/**
 * Convert a trace offset to absolute simulation time.
 *
 * @param experimentStartMs Common trace epoch in milliseconds.
 * @param traceTimeMs Trace offset in milliseconds.
 * @return Absolute simulation time.
 */
Time GetScheduledSimulationTime(uint64_t experimentStartMs, double traceTimeMs);

/**
 * Get the absolute one-second bucket containing a simulation time.
 *
 * @param simulationTime Absolute simulation time.
 * @return Zero-based absolute second.
 */
uint32_t GetAbsoluteSecond(Time simulationTime);

} // namespace ns3

#endif // TRAFFIC_SCHEDULE_H
