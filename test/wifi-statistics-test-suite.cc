#include "llm-test-suite.h"

#include "../examples/wifi-statistics.h"

#include <vector>

using namespace ns3;

namespace
{

/**
 * @ingroup tests
 *
 * Verify statistics window boundaries.
 */
class StatisticsWindowTestCase : public TestCase
{
  public:
    StatisticsWindowTestCase();

  private:
    void DoRun() override;
};

StatisticsWindowTestCase::StatisticsWindowTestCase()
    : TestCase("calculate statistics window boundaries")
{
}

void
StatisticsWindowTestCase::DoRun()
{
    uint32_t index = 999;
    NS_TEST_ASSERT_MSG_EQ(GetStatisticsWindowIndex(1000000, 1000000, 25.0, 10, index),
                          true,
                          "Epoch must be included");
    NS_TEST_ASSERT_MSG_EQ(index, 0, "Wrong first window");
    NS_TEST_ASSERT_MSG_EQ(GetStatisticsWindowIndex(1009999, 1000000, 25.0, 10, index),
                          true,
                          "Last microsecond of first window must be included");
    NS_TEST_ASSERT_MSG_EQ(index, 0, "Wrong boundary window");
    NS_TEST_ASSERT_MSG_EQ(GetStatisticsWindowIndex(1010000, 1000000, 25.0, 10, index),
                          true,
                          "Second window must be included");
    NS_TEST_ASSERT_MSG_EQ(index, 1, "Wrong second window");
    NS_TEST_ASSERT_MSG_EQ(GetStatisticsWindowIndex(1025000, 1000000, 25.0, 10, index),
                          false,
                          "End boundary must be excluded");
}

/**
 * @ingroup tests
 *
 * Verify airtime-weighted PHY rates.
 */
class PhyRateAccumulatorTestCase : public TestCase
{
  public:
    PhyRateAccumulatorTestCase();

  private:
    void DoRun() override;
};

PhyRateAccumulatorTestCase::PhyRateAccumulatorTestCase()
    : TestCase("calculate airtime-weighted PHY rates")
{
}

void
PhyRateAccumulatorTestCase::DoRun()
{
    PhyRateAccumulator rates;
    rates.Add(10e6, 100.0);
    rates.Add(20e6, 300.0);

    NS_TEST_ASSERT_MSG_EQ(rates.txAttempts, 2, "Wrong transmit attempt count");
    NS_TEST_ASSERT_MSG_EQ_TOL(rates.AverageMbps(),
                              17.5,
                              1e-9,
                              "Wrong weighted PHY rate");
}

} // namespace

std::vector<TestCase*>
CreateWifiStatisticsTestCases()
{
    return {new StatisticsWindowTestCase, new PhyRateAccumulatorTestCase};
}
