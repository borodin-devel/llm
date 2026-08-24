#include "llm-test-suite.h"

#include "ns3/inet-socket-address.h"
#include "ns3/traffic-schedule.h"

#include <vector>

using namespace ns3;

namespace
{

/**
 * @ingroup tests
 *
 * Verify conversion and ordering of legacy traffic operations.
 */
class TrafficScheduleTestCase : public TestCase
{
  public:
    TrafficScheduleTestCase();

  private:
    void DoRun() override;
};

TrafficScheduleTestCase::TrafficScheduleTestCase()
    : TestCase("convert and order traffic operations")
{
}

void
TrafficScheduleTestCase::DoRun()
{
    LegacyAgentOperations input;
    input["a"] = {
        {10, 30.0, 30.0, 9},
        {15, 10.0, 35.0, 8},
    };
    input["b"] = {
        {20, 40.0, 10.0, 11},
    };

    const auto uplink = BuildUplinkSchedule(input);
    NS_TEST_ASSERT_MSG_EQ(uplink.size(), 3, "Unexpected uplink count");
    NS_TEST_ASSERT_MSG_EQ_TOL(uplink[0].traceTimeMs, 10.0, 1e-9, "Wrong first UL time");
    NS_TEST_ASSERT_MSG_EQ(uplink[0].payloadBytes, 11, "Wrong first UL bytes");
    NS_TEST_ASSERT_MSG_EQ(uplink[0].agentKey, "b", "Wrong first UL agent");
    NS_TEST_ASSERT_MSG_EQ_TOL(uplink[1].traceTimeMs, 30.0, 1e-9, "Wrong second UL time");
    NS_TEST_ASSERT_MSG_EQ_TOL(uplink[2].traceTimeMs, 35.0, 1e-9, "Wrong third UL time");

    const Address station = InetSocketAddress(Ipv4Address("10.1.0.2"), 9000);
    const std::map<std::string, Address> stationAddressByAgent = {{"a", station}};
    const auto downlink = BuildDownlinkSchedules(input, stationAddressByAgent);

    NS_TEST_ASSERT_MSG_EQ(downlink.size(), 1, "Agent without a station was not skipped");
    const auto& stationSchedule = downlink.at(station);
    NS_TEST_ASSERT_MSG_EQ(stationSchedule.size(), 2, "Wrong downlink count");
    NS_TEST_ASSERT_MSG_EQ_TOL(stationSchedule[0].traceTimeMs,
                              10.0,
                              1e-9,
                              "Wrong first DL time");
    NS_TEST_ASSERT_MSG_EQ(stationSchedule[0].payloadBytes, 15, "Wrong first DL bytes");
    NS_TEST_ASSERT_MSG_EQ_TOL(stationSchedule[1].traceTimeMs,
                              30.0,
                              1e-9,
                              "Wrong second DL time");
    NS_TEST_ASSERT_MSG_EQ(stationSchedule[1].payloadBytes, 10, "Wrong second DL bytes");
}

/**
 * @ingroup tests
 *
 * Verify simulation-time conversion and absolute-second bucketing.
 */
class TrafficScheduleTimeTestCase : public TestCase
{
  public:
    TrafficScheduleTimeTestCase();

  private:
    void DoRun() override;
};

TrafficScheduleTimeTestCase::TrafficScheduleTimeTestCase()
    : TestCase("convert trace times to simulation times")
{
}

void
TrafficScheduleTimeTestCase::DoRun()
{
    const Time scheduled = GetScheduledSimulationTime(2000, 12.5);
    NS_TEST_ASSERT_MSG_EQ(scheduled.GetNanoSeconds(), 2012500000, "Wrong scheduled time");
    NS_TEST_ASSERT_MSG_EQ(GetAbsoluteSecond(Seconds(3.999)), 3, "Wrong absolute second");
}

} // namespace

std::vector<TestCase*>
CreateTrafficScheduleTestCases()
{
    return {new TrafficScheduleTestCase, new TrafficScheduleTimeTestCase};
}
