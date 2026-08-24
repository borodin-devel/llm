#include "llm-log.h"

namespace ns3::llm_detail
{

LogComponent&
GetAgentDistributionLog()
{
    static LogComponent component("AgentDistribution", __FILE__);
    return component;
}

LogComponent&
GetContentionAwareDistributionLog()
{
    static LogComponent component("ContentionAwareAgentDistribution", __FILE__);
    return component;
}

} // namespace ns3::llm_detail
