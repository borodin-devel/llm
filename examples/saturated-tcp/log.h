#ifndef SATURATED_TCP_LOG_H
#define SATURATED_TCP_LOG_H

#include <string_view>

namespace ns3
{

class LogComponent;

namespace saturated_tcp_example
{

/** @return Shared saturated-tcp-scenario log component. */
LogComponent& GetScenarioLog();

/**
 * Enable the shared scenario component at a validated configured level.
 *
 * The `off` level leaves the component disabled.
 *
 * @param level Lower-case saturated scenario logging level.
 * @throws std::invalid_argument if @p level is unknown.
 */
void ConfigureScenarioLogging(std::string_view level);

} // namespace saturated_tcp_example
} // namespace ns3

#endif // SATURATED_TCP_LOG_H
