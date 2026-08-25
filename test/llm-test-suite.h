#ifndef LLM_TEST_SUITE_H
#define LLM_TEST_SUITE_H

#include "ns3/test.h"

#include <vector>

std::vector<ns3::TestCase*> CreateTraceParserTestCases();
std::vector<ns3::TestCase*> CreateAgentDistributionTestCases();
std::vector<ns3::TestCase*> CreateAppTxTagTestCases();
std::vector<ns3::TestCase*> CreateTrafficScheduleTestCases();
std::vector<ns3::TestCase*> CreateTrafficCoordinatorTestCases();
std::vector<ns3::TestCase*> CreateWifiStatisticsTestCases();
std::vector<ns3::TestCase*> CreateScenarioConfigTestCases();
std::vector<ns3::TestCase*> CreateScenarioConfigValidationTestCases();
std::vector<ns3::TestCase*> CreateScenarioRunPathTestCases();
std::vector<ns3::TestCase*> CreateScenarioTopologyTestCases();

#endif // LLM_TEST_SUITE_H
