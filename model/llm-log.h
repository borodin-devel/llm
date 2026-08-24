#ifndef LLM_LOG_H
#define LLM_LOG_H

#include "ns3/log.h"

namespace ns3::llm_detail
{

LogComponent& GetAgentDistributionLog();
LogComponent& GetContentionAwareDistributionLog();

} // namespace ns3::llm_detail

#endif // LLM_LOG_H
