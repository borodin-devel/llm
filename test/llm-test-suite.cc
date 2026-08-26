#include "llm-test-suite.h"

using namespace ns3;

namespace
{

/**
 * @ingroup tests
 *
 * Test suite for the llm module.
 */
class LlmTestSuite : public TestSuite
{
  public:
    LlmTestSuite();

  private:
    void AddCases(const std::vector<TestCase*>& testCases);
};

LlmTestSuite::LlmTestSuite()
    : TestSuite("llm", Type::UNIT)
{
    AddCases(CreateTraceParserTestCases());
    AddCases(CreateAgentDistributionTestCases());
    AddCases(CreateAppTxTagTestCases());
    AddCases(CreateTrafficScheduleTestCases());
    AddCases(CreateTrafficCoordinatorTestCases());
    AddCases(CreateExperimentJsonTestCases());
    AddCases(CreateExperimentHierarchyJsonTestCases());
    AddCases(CreateExperimentWindowTestCases());
    AddCases(CreateExperimentAppTestCases());
    AddCases(CreateExperimentTcpTestCases());
    AddCases(CreateExperimentDeviceMacTestCases());
    AddCases(CreateExperimentPhyTestCases());
    AddCases(CreateExperimentSummaryTestCases());
    AddCases(CreateExperimentValidationTestCases());
    AddCases(CreateScenarioConfigTestCases());
    AddCases(CreateScenarioConfigJsonTestCases());
    AddCases(CreateScenarioConfigValidationTestCases());
    AddCases(CreateScenarioLoggingTestCases());
    AddCases(CreateScenarioRunPathTestCases());
    AddCases(CreateScenarioTopologyTestCases());
    AddCases(CreateSaturatedTcpConfigTestCases());
    AddCases(CreateSaturatedTcpPropagationTestCases());
    AddCases(CreateSaturatedTcpAccessWaitTestCases());
    AddCases(CreateSaturatedTcpStaPhyMetricTestCases());
    AddCases(CreateSaturatedTcpBenchmarkOutputTestCases());
    AddCases(CreateSaturatedTcpTrafficTestCases());
    AddCases(CreateSaturatedTcpTopologyTestCases());
}

void
LlmTestSuite::AddCases(const std::vector<TestCase*>& testCases)
{
    for (auto* testCase : testCases)
    {
        AddTestCase(testCase, TestCase::Duration::QUICK);
    }
}

LlmTestSuite g_llmTestSuite;

} // namespace
