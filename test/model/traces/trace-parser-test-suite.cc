#include "../../llm-test-suite.h"

#include "ns3/agent-distribution.h"
#include "ns3/trace-parser.h"

#include <sstream>
#include <stdexcept>
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

/**
 * @ingroup tests
 *
 * Verify that parsing can use an in-memory stream.
 */
class TraceStreamParserTestCase : public TestCase
{
  public:
    TraceStreamParserTestCase();

  private:
    void DoRun() override;
};

/**
 * @ingroup tests
 *
 * Verify trace-file open failures are reported to the caller.
 */
class TraceFileOpenFailureTestCase : public TestCase
{
  public:
    TraceFileOpenFailureTestCase();

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

TraceStreamParserTestCase::TraceStreamParserTestCase()
    : TestCase("parse an agent trace from a stream")
{
}

TraceFileOpenFailureTestCase::TraceFileOpenFailureTestCase()
    : TestCase("throw trace file open failures with the input path")
{
}

void
TraceStreamParserTestCase::DoRun()
{
    std::istringstream input(R"({
      "traces": [{
        "agentId": 7,
        "agentType": "worker",
        "tasks": [{"operations": [{
          "startOffsetMs": 5.0,
          "durationMs": 15.0,
          "downlinkBytes": 8,
          "uplinkBytes": 9
        }]}]
      }]
    })");

    const ParsedResult parsed = ParseJson(input);

    NS_TEST_ASSERT_MSG_EQ(parsed.agents.size(), 1, "Unexpected agent count");
    NS_TEST_ASSERT_MSG_EQ(parsed.agents[0].key, "7_worker", "Unexpected agent key");
    NS_TEST_ASSERT_MSG_EQ_TOL(parsed.experimentDurationMs, 20.0, 1e-9, "Unexpected trace duration");
}

void
TraceFileOpenFailureTestCase::DoRun()
{
    const std::string missingPath = CreateTempDirFilename("missing/trace.json");
    try
    {
        ParseJsonFile(missingPath);
        NS_TEST_ASSERT_MSG_EQ(true, false, "Missing trace file was accepted");
    }
    catch (const std::runtime_error& error)
    {
        NS_TEST_ASSERT_MSG_NE(std::string(error.what()).find(missingPath),
                              std::string::npos,
                              "Trace open error lacks input path: " << error.what());
    }
}

} // namespace

std::vector<TestCase*>
CreateTraceParserTestCases()
{
    return {new TraceParserTestCase,
            new TraceStreamParserTestCase,
            new TraceFileOpenFailureTestCase};
}
