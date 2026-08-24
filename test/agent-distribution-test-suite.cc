#include "llm-test-suite.h"

#include "ns3/agent-distribution.h"
#include "ns3/contention-aware-agent-distribution.h"
#include "ns3/inet-socket-address.h"

#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using namespace ns3;

namespace
{

ParsedResult
CreateOverlappingAgents()
{
    ParsedResult parsed;
    parsed.agents = {
        {"1_planner", 1, 1, {{100, 0.0, 10.0, 50}}},
        {"2_worker", 2, 2, {{200, 0.0, 10.0, 80}}},
    };
    return parsed;
}

InetSocketAddress
GetStationAddress(const DistributionResult& result, int bssIndex, const std::string& agentKey)
{
    return InetSocketAddress::ConvertFrom(result.apStationMaps.at(bssIndex).at(agentKey));
}

/**
 * @ingroup tests
 *
 * Characterize the existing simple distribution heuristic.
 */
class SimpleDistributionTestCase : public TestCase
{
  public:
    SimpleDistributionTestCase();

  private:
    void DoRun() override;
};

SimpleDistributionTestCase::SimpleDistributionTestCase()
    : TestCase("preserve simple agent distribution")
{
}

void
SimpleDistributionTestCase::DoRun()
{
    const auto result = DistributeAgents(CreateOverlappingAgents(), 2, 2, 1);
    const auto worker = GetStationAddress(result, 0, "2_worker");
    const auto planner = GetStationAddress(result, 1, "1_planner");

    NS_TEST_ASSERT_MSG_EQ(worker.GetIpv4(), Ipv4Address("10.1.0.2"), "Wrong worker STA");
    NS_TEST_ASSERT_MSG_EQ(worker.GetPort(), 9000, "Wrong worker port");
    NS_TEST_ASSERT_MSG_EQ(planner.GetIpv4(), Ipv4Address("10.1.1.2"), "Wrong planner STA");
    NS_TEST_ASSERT_MSG_EQ(planner.GetPort(), 9000, "Wrong planner port");
}

/**
 * @ingroup tests
 *
 * Characterize deterministic contention-aware distribution.
 */
class ContentionAwareDistributionTestCase : public TestCase
{
  public:
    ContentionAwareDistributionTestCase();

  private:
    void DoRun() override;
};

ContentionAwareDistributionTestCase::ContentionAwareDistributionTestCase()
    : TestCase("preserve contention-aware agent distribution")
{
}

void
ContentionAwareDistributionTestCase::DoRun()
{
    ContentionAwareDistributionConfig config;
    config.nAp = 2;
    config.nStationsPerAp = 2;
    config.maxAgentsPerStation = 1;
    config.lowContentionPriority = true;
    config.slotMs = 50;

    const auto parsed = CreateOverlappingAgents();
    const auto first = DistributeAgentsContentionAware(parsed, config);
    const auto second = DistributeAgentsContentionAware(parsed, config);
    const auto planner = GetStationAddress(first, 0, "1_planner");
    const auto worker = GetStationAddress(first, 1, "2_worker");

    NS_TEST_ASSERT_MSG_EQ(planner.GetIpv4(), Ipv4Address("10.1.0.2"), "Wrong planner STA");
    NS_TEST_ASSERT_MSG_EQ(planner.GetPort(), 9000, "Wrong planner port");
    NS_TEST_ASSERT_MSG_EQ(worker.GetIpv4(), Ipv4Address("10.1.1.2"), "Wrong worker STA");
    NS_TEST_ASSERT_MSG_EQ(worker.GetPort(), 9000, "Wrong worker port");
    NS_TEST_ASSERT_MSG_EQ(GetStationAddress(second, 0, "1_planner").GetIpv4(),
                          planner.GetIpv4(),
                          "Planner mapping is not deterministic");
    NS_TEST_ASSERT_MSG_EQ(GetStationAddress(second, 1, "2_worker").GetIpv4(),
                          worker.GetIpv4(),
                          "Worker mapping is not deterministic");
}

/**
 * @ingroup tests
 *
 * Verify contention-aware configuration validation.
 */
class DistributionValidationTestCase : public TestCase
{
  public:
    DistributionValidationTestCase();

  private:
    void DoRun() override;
    void ExpectInvalid(const ContentionAwareDistributionConfig& config, const std::string& name);
};

/**
 * @ingroup tests
 *
 * Characterize slot boundaries and empty input.
 */
class DistributionBoundaryTestCase : public TestCase
{
  public:
    DistributionBoundaryTestCase();

  private:
    void DoRun() override;
};

/**
 * @ingroup tests
 *
 * Verify maximum-station placement when contention priority is disabled.
 */
class MaximumStationDistributionTestCase : public TestCase
{
  public:
    MaximumStationDistributionTestCase();

  private:
    void DoRun() override;
};

MaximumStationDistributionTestCase::MaximumStationDistributionTestCase()
    : TestCase("use maximum stations when contention priority is disabled")
{
}

void
MaximumStationDistributionTestCase::DoRun()
{
    ParsedResult parsed;
    parsed.agents = {
        {"1_alpha", 1, 1, {{100, 0.0, 10.0, 50}}},
        {"2_beta", 2, 2, {{200, 0.0, 10.0, 80}}},
        {"3_gamma", 3, 3, {{300, 0.0, 10.0, 120}}},
    };

    ContentionAwareDistributionConfig config;
    config.nAp = 1;
    config.nStationsPerAp = 3;
    config.maxAgentsPerStation = 2;
    config.lowContentionPriority = false;
    config.slotMs = 50;

    const auto first = DistributeAgentsContentionAware(parsed, config);
    const auto second = DistributeAgentsContentionAware(parsed, config);

    std::set<uint16_t> stationPorts;
    for (const auto& agent : parsed.agents)
    {
        const auto firstAddress = GetStationAddress(first, 0, agent.key);
        const auto secondAddress = GetStationAddress(second, 0, agent.key);
        stationPorts.insert(firstAddress.GetPort());

        NS_TEST_ASSERT_MSG_EQ(firstAddress.GetIpv4(),
                              secondAddress.GetIpv4(),
                              agent.key << " maximum-station IP changed");
        NS_TEST_ASSERT_MSG_EQ(firstAddress.GetPort(),
                              secondAddress.GetPort(),
                              agent.key << " maximum-station port changed");
    }

    NS_TEST_ASSERT_MSG_EQ(stationPorts.size(), 3, "Maximum-station policy did not use every STA");
    NS_TEST_ASSERT_MSG_EQ(stationPorts.contains(9000), true, "STA 0 was not used");
    NS_TEST_ASSERT_MSG_EQ(stationPorts.contains(9001), true, "STA 1 was not used");
    NS_TEST_ASSERT_MSG_EQ(stationPorts.contains(9002), true, "STA 2 was not used");
}

DistributionValidationTestCase::DistributionValidationTestCase()
    : TestCase("reject invalid contention-aware configuration")
{
}

void
DistributionValidationTestCase::ExpectInvalid(const ContentionAwareDistributionConfig& config,
                                              const std::string& name)
{
    bool threw = false;
    try
    {
        (void)DistributeAgentsContentionAware(CreateOverlappingAgents(), config);
    }
    catch (const std::invalid_argument&)
    {
        threw = true;
    }
    NS_TEST_EXPECT_MSG_EQ(threw, true, name << " must be rejected");
}

void
DistributionValidationTestCase::DoRun()
{
    ContentionAwareDistributionConfig config;

    config.nAp = 0;
    ExpectInvalid(config, "zero AP count");

    config = {};
    config.nStationsPerAp = 0;
    ExpectInvalid(config, "zero station count");

    config = {};
    config.maxAgentsPerStation = -1;
    ExpectInvalid(config, "negative station capacity");

    config = {};
    config.slotMs = 0;
    ExpectInvalid(config, "zero slot width");

    config = {};
    config.nAp = 1;
    config.nStationsPerAp = 1;
    config.maxAgentsPerStation = 1;
    ExpectInvalid(config, "insufficient capacity");
}

DistributionBoundaryTestCase::DistributionBoundaryTestCase()
    : TestCase("preserve distribution slot boundaries")
{
}

void
DistributionBoundaryTestCase::DoRun()
{
    ParsedResult parsed;
    parsed.agents = {
        {"1_before", 1, 1, {{10, 49.999, 55.0, 10}}},
        {"2_boundary", 2, 2, {{10, 50.0, 60.0, 10}}},
        {"3_next", 3, 3, {{10, 100.0, 110.0, 10}}},
    };

    ContentionAwareDistributionConfig config;
    config.nAp = 2;
    config.nStationsPerAp = 3;
    config.maxAgentsPerStation = 2;
    config.lowContentionPriority = true;
    config.slotMs = 50;

    const auto first = DistributeAgentsContentionAware(parsed, config);
    const auto second = DistributeAgentsContentionAware(parsed, config);

    for (const auto& agent : parsed.agents)
    {
        int firstCount = 0;
        int secondCount = 0;
        Address firstAddress;
        Address secondAddress;

        for (int bssIndex = 0; bssIndex < config.nAp; ++bssIndex)
        {
            const auto firstIt = first.apStationMaps[bssIndex].find(agent.key);
            if (firstIt != first.apStationMaps[bssIndex].end())
            {
                ++firstCount;
                firstAddress = firstIt->second;
            }
            const auto secondIt = second.apStationMaps[bssIndex].find(agent.key);
            if (secondIt != second.apStationMaps[bssIndex].end())
            {
                ++secondCount;
                secondAddress = secondIt->second;
            }
        }

        NS_TEST_ASSERT_MSG_EQ(firstCount, 1, agent.key << " was not assigned exactly once");
        NS_TEST_ASSERT_MSG_EQ(secondCount, 1, agent.key << " changed assignment count");
        NS_TEST_ASSERT_MSG_EQ(firstAddress, secondAddress, agent.key << " mapping changed");
    }

    ParsedResult empty;
    const auto emptyResult = DistributeAgentsContentionAware(empty, config);
    NS_TEST_ASSERT_MSG_EQ(emptyResult.apAgentMaps.size(), 2, "Wrong empty AP map count");
    NS_TEST_ASSERT_MSG_EQ(emptyResult.apStationMaps.size(), 2, "Wrong empty STA map count");
    NS_TEST_ASSERT_MSG_EQ(emptyResult.apAgentMaps[0].empty(), true, "Unexpected empty agent");
    NS_TEST_ASSERT_MSG_EQ(emptyResult.apAgentMaps[1].empty(), true, "Unexpected empty agent");
}

} // namespace

std::vector<TestCase*>
CreateAgentDistributionTestCases()
{
    return {new SimpleDistributionTestCase,
            new ContentionAwareDistributionTestCase,
            new MaximumStationDistributionTestCase,
            new DistributionValidationTestCase,
            new DistributionBoundaryTestCase};
}
