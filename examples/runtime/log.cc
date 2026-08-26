#include "log.h"

// The private header above shares the core header's basename.
// clang-format off
#include "ns3/log.h"
// clang-format on

namespace ns3::llm_example
{

LogComponent&
GetScenarioLog()
{
    static LogComponent component("SampleScenario", __FILE__);
    return component;
}

} // namespace ns3::llm_example
