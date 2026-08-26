#ifndef SCENARIO_LOG_H
#define SCENARIO_LOG_H

namespace ns3
{
class LogComponent;
}

namespace ns3::llm_example
{

/** @return Shared llm-scenario log component. */
LogComponent& GetScenarioLog();

} // namespace ns3::llm_example

#endif // SCENARIO_LOG_H
