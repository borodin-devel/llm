#ifndef TRACE_PARSER_H
#define TRACE_PARSER_H

#include "agent-data.h"

#include <istream>
#include <string>

namespace ns3
{

/**
 * Parse agent traces from a JSON stream.
 *
 * @param input JSON input stream.
 * @return Parsed agent operations and complete trace duration.
 */
ParsedResult ParseJson(std::istream& input);

/**
 * Parse agent traces from a JSON file.
 *
 * @param jsonPath Path to the JSON input file.
 * @return Parsed agent operations and complete trace duration.
 */
ParsedResult ParseJsonFile(const std::string& jsonPath);

} // namespace ns3

#endif // TRACE_PARSER_H
