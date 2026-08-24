#include "llm-test-suite.h"

#include "ns3/agent-distribution.h"

#include <string>
#include <vector>

using namespace ns3;

namespace
{

/**
 * @ingroup tests
 *
 * Characterize parsing of network and local operations.
 */
class TraceParserTestCase : public TestCase
{
  public:
    TraceParserTestCase();

  private:
    void DoRun() override;
};

TraceParserTestCase::TraceParserTestCase()
    : TestCase("parse agent traces and retain the complete duration")
{
}

void
TraceParserTestCase::DoRun()
{
    const auto tracePath = std::string(NS_TEST_SOURCEDIR) + "/data/minimal-trace.json";
    const ParsedResult parsed = ParseJsonFile(tracePath);

    NS_TEST_ASSERT_MSG_EQ(parsed.agents.size(), 2, "Unexpected agent count");
    NS_TEST_ASSERT_MSG_EQ_TOL(parsed.experimentDurationMs,
                              125.0,
                              1e-9,
                              "Filtered local operation must extend duration");
    NS_TEST_ASSERT_MSG_EQ(parsed.agents[0].key, "1_planner", "Unexpected first key");
    NS_TEST_ASSERT_MSG_EQ(parsed.agents[0].type, 1, "Unexpected planner type");
    NS_TEST_ASSERT_MSG_EQ(parsed.agents[0].operations.size(),
                          1,
                          "Zero-byte operation must be filtered");
    NS_TEST_ASSERT_MSG_EQ(parsed.agents[1].key, "2_worker", "Unexpected second key");
    NS_TEST_ASSERT_MSG_EQ(parsed.agents[1].type, 2, "Unexpected worker type");
}

} // namespace

std::vector<TestCase*>
CreateTraceParserTestCases()
{
    return {new TraceParserTestCase};
}
