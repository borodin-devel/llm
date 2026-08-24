#include "traffic-schedule.h"

#include <algorithm>
#include <cmath>

namespace ns3
{

std::vector<ScheduledPayload>
BuildUplinkSchedule(const LegacyAgentOperations& operationsByAgent)
{
    std::vector<ScheduledPayload> schedule;

    for (const auto& [agentKey, operations] : operationsByAgent)
    {
        for (const auto& [downlinkBytes, endMs, startOffsetMs, uplinkBytes] : operations)
        {
            (void)downlinkBytes;
            (void)endMs;
            schedule.push_back({agentKey, static_cast<uint32_t>(uplinkBytes), startOffsetMs});
        }
    }

    std::sort(schedule.begin(),
              schedule.end(),
              [](const ScheduledPayload& lhs, const ScheduledPayload& rhs) {
                  return lhs.traceTimeMs < rhs.traceTimeMs;
              });
    return schedule;
}

DownlinkSchedulesByStation
BuildDownlinkSchedules(const LegacyAgentOperations& operationsByAgent,
                       const std::map<std::string, Address>& stationAddressByAgent)
{
    DownlinkSchedulesByStation schedules;

    for (const auto& [agentKey, operations] : operationsByAgent)
    {
        const auto station = stationAddressByAgent.find(agentKey);
        if (station == stationAddressByAgent.end())
        {
            continue;
        }

        for (const auto& [downlinkBytes, endMs, startOffsetMs, uplinkBytes] : operations)
        {
            (void)startOffsetMs;
            (void)uplinkBytes;
            schedules[station->second].push_back(
                {agentKey, static_cast<uint32_t>(downlinkBytes), endMs});
        }
    }

    for (auto& [stationAddress, schedule] : schedules)
    {
        (void)stationAddress;
        std::sort(schedule.begin(),
                  schedule.end(),
                  [](const ScheduledPayload& lhs, const ScheduledPayload& rhs) {
                      return lhs.traceTimeMs < rhs.traceTimeMs;
                  });
    }
    return schedules;
}

Time
GetScheduledSimulationTime(uint64_t experimentStartMs, double traceTimeMs)
{
    return Time::FromDouble(static_cast<double>(experimentStartMs) + traceTimeMs, Time::MS);
}

uint32_t
GetAbsoluteSecond(Time simulationTime)
{
    return static_cast<uint32_t>(std::floor(simulationTime.GetSeconds()));
}

} // namespace ns3
