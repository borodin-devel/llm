#ifndef LLM_LOG_H
#define LLM_LOG_H

#include "ns3/log.h"

namespace ns3::llm_detail
{

/** @return Shared legacy agent-distribution log component. */
LogComponent& GetAgentDistributionLog();

/** @return Shared contention-aware distribution log component. */
LogComponent& GetContentionAwareDistributionLog();

/** @return Shared AP-generator log component. */
LogComponent& GetApGeneratorLog();

/** @return Shared station-generator log component. */
LogComponent& GetStaLlmGeneratorLog();

} // namespace ns3::llm_detail

#endif // LLM_LOG_H
