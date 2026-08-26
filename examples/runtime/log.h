#ifndef SCENARIO_LOG_H
#define SCENARIO_LOG_H

#include "ns3/log.h"

namespace ns3::llm_example
{

/** @return Shared llm-scenario log component. */
LogComponent& GetScenarioLog();

} // namespace ns3::llm_example

#endif // SCENARIO_LOG_H
