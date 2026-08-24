#include "../examples/traffic-coordinator.h"
#include "llm-test-suite.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

#ifdef __unix__
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace ns3;

namespace
{

/**
 * @ingroup tests
 *
 * Verify the integer-second epoch used by the global traffic barrier.
 */
class TrafficEpochTestCase : public TestCase
{
  public:
    TrafficEpochTestCase();

  private:
    void DoRun() override;
};

TrafficEpochTestCase::TrafficEpochTestCase()
    : TestCase("select the next integer-second traffic epoch")
{
}

void
TrafficEpochTestCase::DoRun()
{
    NS_TEST_ASSERT_MSG_EQ(GetNextIntegerSecondMs(0), 1000, "Wrong epoch at zero");
    NS_TEST_ASSERT_MSG_EQ(GetNextIntegerSecondMs(999), 1000, "Wrong epoch below boundary");
    NS_TEST_ASSERT_MSG_EQ(GetNextIntegerSecondMs(1000), 2000, "Wrong epoch at boundary");
    NS_TEST_ASSERT_MSG_EQ(GetNextIntegerSecondMs(1999), 2000, "Wrong epoch above boundary");
}

/**
 * @ingroup tests
 *
 * Verify the no-generator barrier invariant used by FinalizeRegistration().
 */
class TrafficRegistrationInvariantTestCase : public TestCase
{
  public:
    TrafficRegistrationInvariantTestCase();

  private:
    void DoRun() override;
};

TrafficRegistrationInvariantTestCase::TrafficRegistrationInvariantTestCase()
    : TestCase("validate traffic generator registration count")
{
}

void
TrafficRegistrationInvariantTestCase::DoRun()
{
#ifdef __unix__
    const pid_t child = fork();
    NS_TEST_ASSERT_MSG_GT_OR_EQ(child, 0, "Could not create fatal-path child process");

    if (child == 0)
    {
        if (!std::freopen("/dev/null", "w", stderr))
        {
            std::_Exit(EXIT_FAILURE);
        }
        TrafficCoordinator coordinator(10.0, 10.0);
        coordinator.FinalizeRegistration();
        std::_Exit(EXIT_SUCCESS);
    }

    int status = 0;
    NS_TEST_ASSERT_MSG_EQ(waitpid(child, &status, 0), child, "Could not wait for fatal-path child");
    const bool terminatedWithFailure =
        WIFSIGNALED(status) || (WIFEXITED(status) && WEXITSTATUS(status) != EXIT_SUCCESS);
    NS_TEST_ASSERT_MSG_EQ(terminatedWithFailure,
                          true,
                          "FinalizeRegistration accepted an empty traffic barrier");
#endif
}

} // namespace

std::vector<TestCase*>
CreateTrafficCoordinatorTestCases()
{
    return {new TrafficEpochTestCase, new TrafficRegistrationInvariantTestCase};
}
