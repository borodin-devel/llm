#include "../../examples/statistics/types.h"
#include "../llm-test-suite.h"

#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

using namespace ns3;

namespace
{

/**
 * @ingroup tests
 *
 * Verify deterministic experiment entity inventory and duplicate rejection.
 */
class ExperimentEntityRegistryTestCase : public TestCase
{
  public:
    ExperimentEntityRegistryTestCase();

  private:
    void DoRun() override;
};

ExperimentEntityRegistryTestCase::ExperimentEntityRegistryTestCase()
    : TestCase("register deterministic experiment entity identities")
{
}

void
ExperimentEntityRegistryTestCase::DoRun()
{
    ExperimentEntityRegistry registry;
    registry.RegisterAccessPoint(0, 7, "AP0", "10.1.0.1");
    registry.RegisterStation(0, 0, 8, "AP0/STA0", "10.1.0.2");
    registry.RegisterStation(0, 1, 9, "AP0/STA1", "10.1.0.3");

    const auto& accessPoints = registry.GetAccessPoints();
    const auto& stations = registry.GetStations();
    NS_TEST_ASSERT_MSG_EQ(accessPoints.size(), 1, "Wrong AP inventory size");
    NS_TEST_ASSERT_MSG_EQ(stations.size(), 2, "Wrong station inventory size");
    NS_TEST_ASSERT_MSG_EQ(accessPoints.at(0).accessPointId, 0, "Wrong AP inventory order");
    NS_TEST_ASSERT_MSG_EQ(accessPoints.at(0).stationIndex.has_value(), false, "AP has STA index");
    NS_TEST_ASSERT_MSG_EQ(stations.at(0).accessPointId, 0, "Wrong first station AP");
    NS_TEST_ASSERT_MSG_EQ(stations.at(0).stationIndex.value(), 0, "Wrong first station index");
    NS_TEST_ASSERT_MSG_EQ(stations.at(1).stationIndex.value(), 1, "Wrong second station index");

    const auto* accessPoint = registry.FindByNodeId(7);
    NS_TEST_ASSERT_MSG_EQ(accessPoint != nullptr, true, "AP node lookup failed");
    NS_TEST_ASSERT_MSG_EQ(accessPoint->ipv4, "10.1.0.1", "Wrong AP node lookup");
    const auto* station = registry.FindByIpv4("10.1.0.3");
    NS_TEST_ASSERT_MSG_EQ(station != nullptr, true, "Station IPv4 lookup failed");
    NS_TEST_ASSERT_MSG_EQ(station->nodeId, 9, "Wrong station IPv4 lookup");

    bool duplicateNodeRejected = false;
    try
    {
        registry.RegisterAccessPoint(1, 7, "AP1", "10.1.1.1");
    }
    catch (const std::invalid_argument&)
    {
        duplicateNodeRejected = true;
    }
    NS_TEST_ASSERT_MSG_EQ(duplicateNodeRejected, true, "Duplicate node ID was accepted");

    bool duplicateIpv4Rejected = false;
    try
    {
        registry.RegisterAccessPoint(1, 10, "AP1", "10.1.0.1");
    }
    catch (const std::invalid_argument&)
    {
        duplicateIpv4Rejected = true;
    }
    NS_TEST_ASSERT_MSG_EQ(duplicateIpv4Rejected, true, "Duplicate IPv4 address was accepted");

    bool duplicateAccessPointRejected = false;
    try
    {
        registry.RegisterAccessPoint(0, 10, "AP0", "10.1.1.1");
    }
    catch (const std::invalid_argument&)
    {
        duplicateAccessPointRejected = true;
    }
    NS_TEST_ASSERT_MSG_EQ(duplicateAccessPointRejected, true, "Duplicate AP identity was accepted");

    bool duplicateStationRejected = false;
    try
    {
        registry.RegisterStation(0, 0, 10, "AP0/STA0", "10.1.1.1");
    }
    catch (const std::invalid_argument&)
    {
        duplicateStationRejected = true;
    }
    NS_TEST_ASSERT_MSG_EQ(duplicateStationRejected, true, "Duplicate STA identity was accepted");

    ExperimentEntityRegistry unsortedRegistry;
    unsortedRegistry.RegisterAccessPoint(1, 11, "AP1", "10.1.1.1");
    unsortedRegistry.RegisterAccessPoint(0, 10, "AP0", "10.1.0.1");
    unsortedRegistry.RegisterStation(1, 1, 13, "AP1/STA1", "10.1.1.3");
    unsortedRegistry.RegisterStation(0, 1, 12, "AP0/STA1", "10.1.0.2");
    unsortedRegistry.RegisterStation(0, 0, 14, "AP0/STA0", "10.1.0.3");
    NS_TEST_ASSERT_MSG_EQ(unsortedRegistry.GetAccessPoints().at(0).accessPointId,
                          0,
                          "AP ordering depends on registration order");
    NS_TEST_ASSERT_MSG_EQ(unsortedRegistry.GetStations().at(0).accessPointId,
                          0,
                          "Station AP ordering depends on registration order");
    NS_TEST_ASSERT_MSG_EQ(unsortedRegistry.GetStations().at(0).stationIndex.value(),
                          0,
                          "Station index ordering depends on registration order");
    NS_TEST_ASSERT_MSG_EQ(unsortedRegistry.GetStations().at(1).stationIndex.value(),
                          1,
                          "Second station index ordering is incorrect");
}

/**
 * @ingroup tests
 *
 * Verify experiment-window resolution, partial duration, and interval splitting.
 */
class ExperimentWindowTestCase : public TestCase
{
  public:
    ExperimentWindowTestCase();

  private:
    void DoRun() override;
};

ExperimentWindowTestCase::ExperimentWindowTestCase()
    : TestCase("resolve and split experiment statistics windows")
{
}

void
ExperimentWindowTestCase::DoRun()
{
    ExperimentWindowBounds bounds{};
    NS_TEST_ASSERT_MSG_EQ(ResolveExperimentWindow(0, 60000, 25000, bounds),
                          true,
                          "First relative time was rejected");
    NS_TEST_ASSERT_MSG_EQ(bounds.index, 0, "Wrong first window index");
    NS_TEST_ASSERT_MSG_EQ(bounds.startUs, 0, "Wrong first window start");
    NS_TEST_ASSERT_MSG_EQ(bounds.durationUs, 25000, "Wrong first window duration");

    NS_TEST_ASSERT_MSG_EQ(ResolveExperimentWindow(24999, 60000, 25000, bounds),
                          true,
                          "First window end was rejected");
    NS_TEST_ASSERT_MSG_EQ(bounds.index, 0, "Wrong first-window endpoint index");
    NS_TEST_ASSERT_MSG_EQ(ResolveExperimentWindow(25000, 60000, 25000, bounds),
                          true,
                          "Second window start was rejected");
    NS_TEST_ASSERT_MSG_EQ(bounds.index, 1, "Wrong second window index");
    NS_TEST_ASSERT_MSG_EQ(bounds.startUs, 25000, "Wrong second window start");
    NS_TEST_ASSERT_MSG_EQ(bounds.durationUs, 25000, "Wrong second window duration");
    NS_TEST_ASSERT_MSG_EQ(ResolveExperimentWindow(59999, 60000, 25000, bounds),
                          true,
                          "Final window endpoint was rejected");
    NS_TEST_ASSERT_MSG_EQ(bounds.index, 2, "Wrong final window index");
    NS_TEST_ASSERT_MSG_EQ(bounds.startUs, 50000, "Wrong final window start");
    NS_TEST_ASSERT_MSG_EQ(bounds.durationUs, 10000, "Wrong final window duration");
    NS_TEST_ASSERT_MSG_EQ(ResolveExperimentWindow(60000, 60000, 25000, bounds),
                          false,
                          "Experiment end boundary was included");

    constexpr uint64_t largeIndex = UINT64_C(1) << 32;
    const int64_t largeRelativeUs = static_cast<int64_t>(largeIndex * 25000);
    NS_TEST_ASSERT_MSG_EQ(
        ResolveExperimentWindow(largeRelativeUs, largeRelativeUs + 25000, 25000, bounds),
        true,
        "Large window index was rejected");
    NS_TEST_ASSERT_MSG_EQ(bounds.index, largeIndex, "Window index narrowed below 64 bits");

    const auto pieces = SplitExperimentInterval(20000, 55000, 60000, 25000);
    NS_TEST_ASSERT_MSG_EQ(pieces.size(), 3, "Wrong split piece count");
    NS_TEST_ASSERT_MSG_EQ(pieces.at(0).first, 0, "Wrong first split window");
    NS_TEST_ASSERT_MSG_EQ(pieces.at(0).second, 5000, "Wrong first split duration");
    NS_TEST_ASSERT_MSG_EQ(pieces.at(1).first, 1, "Wrong second split window");
    NS_TEST_ASSERT_MSG_EQ(pieces.at(1).second, 25000, "Wrong second split duration");
    NS_TEST_ASSERT_MSG_EQ(pieces.at(2).first, 2, "Wrong third split window");
    NS_TEST_ASSERT_MSG_EQ(pieces.at(2).second, 5000, "Wrong third split duration");
    NS_TEST_ASSERT_MSG_EQ(SplitExperimentInterval(-1000, 0, 60000, 25000).empty(),
                          true,
                          "Empty clipped interval produced pieces");
    NS_TEST_ASSERT_MSG_EQ(SplitExperimentInterval(60000, 70000, 60000, 25000).empty(),
                          true,
                          "Out-of-range interval produced pieces");
}

/**
 * @ingroup tests
 *
 * Verify sample merging and directional value selection.
 */
class ExperimentStatisticsPrimitiveTestCase : public TestCase
{
  public:
    ExperimentStatisticsPrimitiveTestCase();

  private:
    void DoRun() override;
};

ExperimentStatisticsPrimitiveTestCase::ExperimentStatisticsPrimitiveTestCase()
    : TestCase("accumulate shared samples and directions")
{
}

void
ExperimentStatisticsPrimitiveTestCase::DoRun()
{
    SampleAccumulator samples;
    samples.Add(4.0);
    SampleAccumulator other;
    other.Add(6.0);
    samples.Merge(other);
    NS_TEST_ASSERT_MSG_EQ(samples.count, 2, "Wrong sample count");
    NS_TEST_ASSERT_MSG_EQ(samples.sum, 10.0, "Wrong sample sum");
    NS_TEST_ASSERT_MSG_EQ(samples.sumSquares, 52.0, "Wrong squared sample sum");
    NS_TEST_ASSERT_MSG_EQ(samples.minimum, 4.0, "Wrong sample minimum");
    NS_TEST_ASSERT_MSG_EQ(samples.maximum, 6.0, "Wrong sample maximum");

    DirectionPair<uint64_t> directions{};
    directions.Get(ExperimentDirection::UPLINK) = 3;
    directions.Get(ExperimentDirection::DOWNLINK) = 5;
    NS_TEST_ASSERT_MSG_EQ(directions.Get(ExperimentDirection::UPLINK), 3, "Wrong uplink value");
    NS_TEST_ASSERT_MSG_EQ(directions.Get(ExperimentDirection::DOWNLINK), 5, "Wrong downlink value");
}

/**
 * @ingroup tests
 *
 * Verify negative sample extrema survive additions and empty merges.
 */
class SampleAccumulatorNegativeValuesTestCase : public TestCase
{
  public:
    SampleAccumulatorNegativeValuesTestCase();

  private:
    void DoRun() override;
};

SampleAccumulatorNegativeValuesTestCase::SampleAccumulatorNegativeValuesTestCase()
    : TestCase("preserve negative sample extrema across merges")
{
}

void
SampleAccumulatorNegativeValuesTestCase::DoRun()
{
    SampleAccumulator negativeSamples;
    negativeSamples.Add(-3.0);
    NS_TEST_ASSERT_MSG_EQ(negativeSamples.count, 1, "Wrong single negative sample count");
    NS_TEST_ASSERT_MSG_EQ(negativeSamples.sum, -3.0, "Wrong single negative sample sum");
    NS_TEST_ASSERT_MSG_EQ(negativeSamples.sumSquares, 9.0, "Wrong single negative sample square");
    NS_TEST_ASSERT_MSG_EQ(negativeSamples.minimum, -3.0, "Wrong single negative sample minimum");
    NS_TEST_ASSERT_MSG_EQ(negativeSamples.maximum, -3.0, "Wrong single negative sample maximum");

    negativeSamples.Add(-8.0);
    NS_TEST_ASSERT_MSG_EQ(negativeSamples.count, 2, "Wrong negative sample count");
    NS_TEST_ASSERT_MSG_EQ(negativeSamples.sum, -11.0, "Wrong negative sample sum");
    NS_TEST_ASSERT_MSG_EQ(negativeSamples.sumSquares, 73.0, "Wrong negative squared sample sum");
    NS_TEST_ASSERT_MSG_EQ(negativeSamples.minimum, -8.0, "Wrong negative sample minimum");
    NS_TEST_ASSERT_MSG_EQ(negativeSamples.maximum, -3.0, "Wrong negative sample maximum");

    SampleAccumulator emptyDestination;
    emptyDestination.Merge(negativeSamples);
    NS_TEST_ASSERT_MSG_EQ(emptyDestination.count, 2, "Empty destination lost merged count");
    NS_TEST_ASSERT_MSG_EQ(emptyDestination.sum, -11.0, "Empty destination lost merged sum");
    NS_TEST_ASSERT_MSG_EQ(emptyDestination.sumSquares,
                          73.0,
                          "Empty destination lost merged squares");
    NS_TEST_ASSERT_MSG_EQ(emptyDestination.minimum, -8.0, "Empty destination lost merged minimum");
    NS_TEST_ASSERT_MSG_EQ(emptyDestination.maximum, -3.0, "Empty destination lost merged maximum");

    SampleAccumulator nonemptyDestination;
    nonemptyDestination.Add(-5.0);
    nonemptyDestination.Merge(negativeSamples);
    NS_TEST_ASSERT_MSG_EQ(nonemptyDestination.count, 3, "Nonempty destination lost merged count");
    NS_TEST_ASSERT_MSG_EQ(nonemptyDestination.sum, -16.0, "Nonempty destination lost merged sum");
    NS_TEST_ASSERT_MSG_EQ(nonemptyDestination.sumSquares,
                          98.0,
                          "Nonempty destination lost merged squares");
    NS_TEST_ASSERT_MSG_EQ(nonemptyDestination.minimum,
                          -8.0,
                          "Nonempty destination lost merged minimum");
    NS_TEST_ASSERT_MSG_EQ(nonemptyDestination.maximum,
                          -3.0,
                          "Nonempty destination lost merged maximum");

    SampleAccumulator emptySource;
    negativeSamples.Merge(emptySource);
    NS_TEST_ASSERT_MSG_EQ(negativeSamples.count, 2, "Empty merge changed count");
    NS_TEST_ASSERT_MSG_EQ(negativeSamples.sum, -11.0, "Empty merge changed sum");
    NS_TEST_ASSERT_MSG_EQ(negativeSamples.sumSquares, 73.0, "Empty merge changed squares");
    NS_TEST_ASSERT_MSG_EQ(negativeSamples.minimum, -8.0, "Empty merge changed minimum");
    NS_TEST_ASSERT_MSG_EQ(negativeSamples.maximum, -3.0, "Empty merge changed maximum");
}

} // namespace

std::vector<TestCase*>
CreateExperimentWindowTestCases()
{
    return {new ExperimentEntityRegistryTestCase,
            new ExperimentWindowTestCase,
            new ExperimentStatisticsPrimitiveTestCase,
            new SampleAccumulatorNegativeValuesTestCase};
}
