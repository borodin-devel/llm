#include "../examples/scenario-config.h"
#include "llm-test-suite.h"

#include <string>
#include <vector>

using namespace ns3;

namespace
{

/**
 * @ingroup tests
 *
 * Verify scenario defaults and accepted argument forms.
 */
class ValidScenarioArgumentsTestCase : public TestCase
{
  public:
    ValidScenarioArgumentsTestCase();

  private:
    void DoRun() override;
};

ValidScenarioArgumentsTestCase::ValidScenarioArgumentsTestCase()
    : TestCase("parse valid scenario arguments")
{
}

void
ValidScenarioArgumentsTestCase::DoRun()
{
    const auto minimal = ParseScenarioArguments({"trace.json"});
    NS_TEST_ASSERT_MSG_EQ(minimal.valid, true, "Minimal arguments rejected");
    NS_TEST_ASSERT_MSG_EQ(minimal.config.tracePath, "trace.json", "Wrong trace path");
    NS_TEST_ASSERT_MSG_EQ(minimal.config.bandwidthMhz, 20, "Wrong default bandwidth");
    NS_TEST_ASSERT_MSG_EQ(minimal.config.statisticsOutputPath,
                          "mac-node-stats.json",
                          "Wrong default output path");
    NS_TEST_ASSERT_MSG_EQ(minimal.config.automaticDuration, true, "Wrong duration mode");

    const auto fixed = ParseScenarioArguments({"trace.json", "80", "stats.json", "2.5"});
    NS_TEST_ASSERT_MSG_EQ(fixed.valid, true, "Fixed arguments rejected");
    NS_TEST_ASSERT_MSG_EQ(fixed.config.bandwidthMhz, 80, "Wrong configured bandwidth");
    NS_TEST_ASSERT_MSG_EQ(fixed.config.statisticsOutputPath,
                          "stats.json",
                          "Wrong configured output path");
    NS_TEST_ASSERT_MSG_EQ(fixed.config.automaticDuration, false, "Wrong fixed duration mode");
    NS_TEST_ASSERT_MSG_EQ_TOL(fixed.config.fixedDurationMs, 2500.0, 1e-9, "Wrong fixed duration");

    const auto automatic = ParseScenarioArguments({"trace.json", "40", "stats.json", "auto"});
    NS_TEST_ASSERT_MSG_EQ(automatic.valid, true, "Automatic duration rejected");
    NS_TEST_ASSERT_MSG_EQ(automatic.config.automaticDuration,
                          true,
                          "Wrong automatic duration mode");
}

/**
 * @ingroup tests
 *
 * Verify scenario argument validation and exact diagnostics.
 */
class InvalidScenarioArgumentsTestCase : public TestCase
{
  public:
    InvalidScenarioArgumentsTestCase();

  private:
    void DoRun() override;
    void CheckInvalidDuration(const std::string& value);
};

InvalidScenarioArgumentsTestCase::InvalidScenarioArgumentsTestCase()
    : TestCase("reject invalid scenario arguments")
{
}

void
InvalidScenarioArgumentsTestCase::CheckInvalidDuration(const std::string& value)
{
    const auto result = ParseScenarioArguments({"trace.json", "20", "stats.json", value});
    NS_TEST_ASSERT_MSG_EQ(result.valid, false, "Invalid duration accepted: " << value);
    NS_TEST_ASSERT_MSG_EQ(result.error,
                          "Invalid experiment_time: " + value +
                              ". Expected 'auto' or a positive number of seconds.",
                          "Wrong duration error");
}

void
InvalidScenarioArgumentsTestCase::DoRun()
{
    const auto missing = ParseScenarioArguments({});
    NS_TEST_ASSERT_MSG_EQ(missing.valid, false, "Missing trace accepted");
    NS_TEST_ASSERT_MSG_EQ(missing.printUsage, true, "Usage not requested");

    const auto invalidBandwidth = ParseScenarioArguments({"trace.json", "30"});
    NS_TEST_ASSERT_MSG_EQ(invalidBandwidth.valid, false, "Invalid bandwidth accepted");
    NS_TEST_ASSERT_MSG_EQ(invalidBandwidth.error,
                          "Unsupported bandwidth: 30 MHz. Expected 20, 40, 80 or 160.",
                          "Wrong bandwidth error");

    const auto tooMany =
        ParseScenarioArguments({"trace.json", "20", "stats.json", "auto", "extra"});
    NS_TEST_ASSERT_MSG_EQ(tooMany.valid, false, "Extra argument accepted");
    NS_TEST_ASSERT_MSG_EQ(tooMany.error,
                          "Too many command-line arguments.",
                          "Wrong extra argument error");

    CheckInvalidDuration("0");
    CheckInvalidDuration("-1");
    CheckInvalidDuration("invalid");
    CheckInvalidDuration("2.5seconds");
}

} // namespace

std::vector<TestCase*>
CreateScenarioConfigTestCases()
{
    return {new ValidScenarioArgumentsTestCase, new InvalidScenarioArgumentsTestCase};
}
