#include "llm-test-suite.h"

#include "../examples/traffic-coordinator.h"

#include <vector>

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

} // namespace

std::vector<TestCase*>
CreateTrafficCoordinatorTestCases()
{
    return {new TrafficEpochTestCase};
}
