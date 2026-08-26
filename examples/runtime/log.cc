#include "log.h"

#include "ns3/core-module.h"

namespace ns3::llm_example
{

LogComponent&
GetScenarioLog()
{
    static LogComponent component("SampleScenario", __FILE__);
    return component;
}

} // namespace ns3::llm_example
