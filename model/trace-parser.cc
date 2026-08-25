#include "trace-parser.h"

#include "llm-log.h"

#include "ns3/json.hpp"

#include <algorithm>
#include <fstream>
#include <map>
#include <numeric>
#include <stdexcept>

using json = nlohmann::json;

namespace ns3
{

static LogComponent& g_log = llm_detail::GetAgentDistributionLog();

ParsedResult
ParseJson(std::istream& input)
{
    const json traceDocument = json::parse(input);

    std::map<std::string, AgentInfo> agentsByKey;
    std::map<std::string, int> typeNumberByName;
    int nextTypeNumber = 1;
    double experimentDurationMs = 0.0;

    for (const auto& agentTrace : traceDocument.at("traces"))
    {
        const int agentId = agentTrace.at("agentId").get<int>();
        const std::string agentType = agentTrace.at("agentType").get<std::string>();

        if (typeNumberByName.find(agentType) == typeNumberByName.end())
        {
            typeNumberByName[agentType] = nextTypeNumber++;
            NS_LOG_INFO("[Parse] Type \"" << agentType << "\" -> " << typeNumberByName[agentType]);
        }

        const std::string key = std::to_string(agentId) + "_" + agentType;
        if (agentsByKey.find(key) == agentsByKey.end())
        {
            agentsByKey[key] = AgentInfo{key, agentId, typeNumberByName[agentType], {}};
        }

        for (const auto& task : agentTrace.at("tasks"))
        {
            for (const auto& operationJson : task.at("operations"))
            {
                const double startOffsetMs = operationJson.at("startOffsetMs").get<double>();
                const double durationMs = operationJson.at("durationMs").get<double>();
                experimentDurationMs = std::max(experimentDurationMs, startOffsetMs + durationMs);

                if (operationJson.at("downlinkBytes").get<int>() <= 0 ||
                    operationJson.at("uplinkBytes").get<int>() <= 0)
                {
                    continue;
                }

                agentsByKey[key].operations.push_back(
                    Operation{operationJson.at("downlinkBytes").get<int>(),
                              startOffsetMs,
                              startOffsetMs + durationMs,
                              operationJson.at("uplinkBytes").get<int>()});
            }
        }
    }

    ParsedResult parsedTrace;
    parsedTrace.experimentDurationMs = experimentDurationMs;
    parsedTrace.agents.reserve(agentsByKey.size());

    for (const auto& [key, agent] : agentsByKey)
    {
        parsedTrace.agents.push_back(agent);
        const int64_t totalBytes =
            std::accumulate(agent.operations.begin(),
                            agent.operations.end(),
                            int64_t{0},
                            [](int64_t sum, const Operation& operation) {
                                return sum + operation.downlinkBytes + operation.uplinkBytes;
                            });
        NS_LOG_INFO("[Parse] Agent key=\"" << key << "\" id=" << agent.id << " type=" << agent.type
                                           << " operations=" << agent.operations.size()
                                           << " bytes=" << totalBytes);
    }

    NS_LOG_INFO("[Parse] Total agents: " << parsedTrace.agents.size());
    NS_LOG_INFO("[Parse] Total types: " << typeNumberByName.size());
    NS_LOG_INFO("[Parse] Experiment duration from JSON: " << parsedTrace.experimentDurationMs
                                                          << " ms");

    return parsedTrace;
}

ParsedResult
ParseJsonFile(const std::string& jsonPath)
{
    NS_LOG_INFO("[Parse] Loading JSON: " << jsonPath);

    std::ifstream input(jsonPath);
    if (!input.is_open())
    {
        throw std::runtime_error("cannot open trace file: '" + jsonPath + "'");
    }
    return ParseJson(input);
}

} // namespace ns3
