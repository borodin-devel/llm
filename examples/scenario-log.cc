#include "scenario-log.h"

namespace ns3::llm_example
{

LogComponent&
GetScenarioLog()
{
    static LogComponent component("SampleScenario", __FILE__);
    return component;
}

} // namespace ns3::llm_example
