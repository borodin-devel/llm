#include "../../examples/saturated-tcp/bss-link-filter.h"
#include "../../examples/saturated-tcp/rssi-placement.h"
#include "../llm-test-suite.h"

#include "ns3/constant-position-mobility-model.h"
#include "ns3/node.h"
#include "ns3/propagation-loss-model.h"

#include <array>
#include <cmath>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace ns3;

namespace
{

/** Deterministic loss model used to expose native delegation. */
class DistanceLossModel : public PropagationLossModel
{
  public:
    /**
     * Get the object TypeId.
     *
     * @return The object TypeId.
     */
    static TypeId GetTypeId();

  private:
    double DoCalcRxPower(double txPowerDbm,
                         Ptr<MobilityModel> sender,
                         Ptr<MobilityModel> receiver) const override;
    int64_t DoAssignStreams(int64_t stream) override;
};

NS_OBJECT_ENSURE_REGISTERED(DistanceLossModel);

TypeId
DistanceLossModel::GetTypeId()
{
    static TypeId tid = TypeId("ns3::DistanceLossModel")
                            .SetParent<PropagationLossModel>()
                            .SetGroupName("Llm")
                            .AddConstructor<DistanceLossModel>();
    return tid;
}

double
DistanceLossModel::DoCalcRxPower(double txPowerDbm,
                                 Ptr<MobilityModel> sender,
                                 Ptr<MobilityModel> receiver) const
{
    return txPowerDbm - sender->GetDistanceFrom(receiver);
}

int64_t
DistanceLossModel::DoAssignStreams(int64_t stream)
{
    return 0;
}

/** Constant loss model used to expose an impossible RSSI bracket. */
class ConstantLossModel : public PropagationLossModel
{
  public:
    /**
     * Get the object TypeId.
     *
     * @return The object TypeId.
     */
    static TypeId GetTypeId();

  private:
    double DoCalcRxPower(double txPowerDbm,
                         Ptr<MobilityModel> sender,
                         Ptr<MobilityModel> receiver) const override;
    int64_t DoAssignStreams(int64_t stream) override;
};

NS_OBJECT_ENSURE_REGISTERED(ConstantLossModel);

TypeId
ConstantLossModel::GetTypeId()
{
    static TypeId tid = TypeId("ns3::ConstantLossModel")
                            .SetParent<PropagationLossModel>()
                            .SetGroupName("Llm")
                            .AddConstructor<ConstantLossModel>();
    return tid;
}

double
ConstantLossModel::DoCalcRxPower(double txPowerDbm,
                                 Ptr<MobilityModel> sender,
                                 Ptr<MobilityModel> receiver) const
{
    return txPowerDbm - 10.0;
}

int64_t
ConstantLossModel::DoAssignStreams(int64_t stream)
{
    return 0;
}

/** Increasing loss model used to expose a non-monotonic native model. */
class IncreasingLossModel : public PropagationLossModel
{
  public:
    /**
     * Get the object TypeId.
     *
     * @return The object TypeId.
     */
    static TypeId GetTypeId();

  private:
    double DoCalcRxPower(double txPowerDbm,
                         Ptr<MobilityModel> sender,
                         Ptr<MobilityModel> receiver) const override;
    int64_t DoAssignStreams(int64_t stream) override;
};

NS_OBJECT_ENSURE_REGISTERED(IncreasingLossModel);

TypeId
IncreasingLossModel::GetTypeId()
{
    static TypeId tid = TypeId("ns3::IncreasingLossModel")
                            .SetParent<PropagationLossModel>()
                            .SetGroupName("Llm")
                            .AddConstructor<IncreasingLossModel>();
    return tid;
}

double
IncreasingLossModel::DoCalcRxPower(double txPowerDbm,
                                   Ptr<MobilityModel> sender,
                                   Ptr<MobilityModel> receiver) const
{
    return txPowerDbm + sender->GetDistanceFrom(receiver);
}

int64_t
IncreasingLossModel::DoAssignStreams(int64_t stream)
{
    return 0;
}

/** Native loss model recording exact power and stream delegation. */
class RecordingLossModel : public PropagationLossModel
{
  public:
    /**
     * Get the object TypeId.
     *
     * @return The object TypeId.
     */
    static TypeId GetTypeId();

    mutable uint32_t powerCalls{0}; ///< Number of received-power calculations.
    uint32_t streamCalls{0};        ///< Number of stream assignments.
    int64_t lastStream{-1};         ///< Most recent stream offset.
    double lossDb{0.0};             ///< Deterministic loss applied in dB.
    int64_t streamCount{0};         ///< Number of streams claimed by this model.

  private:
    double DoCalcRxPower(double txPowerDbm,
                         Ptr<MobilityModel> sender,
                         Ptr<MobilityModel> receiver) const override;
    int64_t DoAssignStreams(int64_t stream) override;
};

NS_OBJECT_ENSURE_REGISTERED(RecordingLossModel);

TypeId
RecordingLossModel::GetTypeId()
{
    static TypeId tid = TypeId("ns3::RecordingLossModel")
                            .SetParent<PropagationLossModel>()
                            .SetGroupName("Llm")
                            .AddConstructor<RecordingLossModel>();
    return tid;
}

double
RecordingLossModel::DoCalcRxPower(double txPowerDbm,
                                  Ptr<MobilityModel> sender,
                                  Ptr<MobilityModel> receiver) const
{
    ++powerCalls;
    return txPowerDbm - lossDb;
}

int64_t
RecordingLossModel::DoAssignStreams(int64_t stream)
{
    ++streamCalls;
    lastStream = stream;
    return streamCount;
}

/**
 * Capture the standard-exception diagnostic from an operation.
 *
 * @param operation Operation expected to fail.
 * @return Exception diagnostic, or an empty string if the operation succeeds.
 */
template <typename Operation>
std::string
CaptureFailure(Operation operation)
{
    try
    {
        operation();
    }
    catch (const std::exception& error)
    {
        return error.what();
    }
    return {};
}

/** Verify native delegation and the complete BSS link-policy matrix. */
class SaturatedTcpLinkPolicyTestCase : public TestCase
{
  public:
    /** Construct the link-policy test. */
    SaturatedTcpLinkPolicyTestCase();

  private:
    void DoRun() override;
};

SaturatedTcpLinkPolicyTestCase::SaturatedTcpLinkPolicyTestCase()
    : TestCase("saturated TCP native propagation link policy")
{
}

void
SaturatedTcpLinkPolicyTestCase::DoRun()
{
    constexpr double txPowerDbm = 20.0;
    std::array<Ptr<Node>, 7> nodes;
    std::array<Ptr<ConstantPositionMobilityModel>, 7> mobility;
    const std::array<double, 7> positions{0.0, 2.0, 5.0, 9.0, 14.0, 20.0, 25.0};
    for (std::size_t i = 0; i < nodes.size(); ++i)
    {
        nodes[i] = CreateObject<Node>();
        mobility[i] = CreateObject<ConstantPositionMobilityModel>();
        mobility[i]->SetPosition(Vector(positions[i], 0.0, 0.0));
        nodes[i]->AggregateObject(mobility[i]);
    }

    auto nativeLoss = CreateObject<DistanceLossModel>();
    auto filter = CreateObject<BssLinkFilterPropagationLossModel>();
    filter->SetNativeLossModel(nativeLoss);
    for (uint32_t bssId = 0; bssId < 3; ++bssId)
    {
        filter->RegisterRadio(mobility[2 * bssId], bssId, SaturatedRadioRole::ACCESS_POINT);
        filter->RegisterRadio(mobility[2 * bssId + 1], bssId, SaturatedRadioRole::STATION);
    }

    const auto expectNative = [&](std::size_t sender, std::size_t receiver) {
        const double expected = txPowerDbm - std::abs(positions[sender] - positions[receiver]);
        NS_TEST_ASSERT_MSG_EQ_TOL(
            filter->CalcRxPower(txPowerDbm, mobility[sender], mobility[receiver]),
            expected,
            1e-12,
            "Allowed link did not preserve native received power");
    };
    expectNative(0, 1);
    expectNative(1, 0);
    expectNative(0, 0);
    expectNative(1, 1);
    expectNative(0, 2);
    expectNative(2, 0);

    const auto expectBlocked = [&](std::size_t sender, std::size_t receiver) {
        const double received =
            filter->CalcRxPower(txPowerDbm, mobility[sender], mobility[receiver]);
        NS_TEST_ASSERT_MSG_EQ(std::isinf(received) && received < 0.0,
                              true,
                              "Cross-BSS link involving a station was not blocked");
    };
    expectBlocked(0, 3);
    expectBlocked(3, 0);
    expectBlocked(1, 2);
    expectBlocked(2, 1);
    expectBlocked(1, 3);
    expectBlocked(3, 1);

    std::ostringstream nodeIdentity;
    nodeIdentity << "node " << nodes[6]->GetId();
    auto diagnostic =
        CaptureFailure([&] { filter->CalcRxPower(txPowerDbm, mobility[0], mobility[6]); });
    NS_TEST_ASSERT_MSG_NE(diagnostic.find(nodeIdentity.str()),
                          std::string::npos,
                          "Unknown receiver diagnostic lacks node identity: " << diagnostic);
    diagnostic = CaptureFailure([&] { filter->CalcRxPower(txPowerDbm, mobility[6], mobility[0]); });
    NS_TEST_ASSERT_MSG_NE(diagnostic.find("mobility"),
                          std::string::npos,
                          "Unknown sender diagnostic lacks mobility identity: " << diagnostic);
    diagnostic = CaptureFailure(
        [&] { filter->RegisterRadio(mobility[0], 0, SaturatedRadioRole::ACCESS_POINT); });
    NS_TEST_ASSERT_MSG_NE(diagnostic.find("already registered"),
                          std::string::npos,
                          "Duplicate registration has wrong diagnostic: " << diagnostic);

    auto missingNative = CreateObject<BssLinkFilterPropagationLossModel>();
    missingNative->RegisterRadio(mobility[0], 0, SaturatedRadioRole::ACCESS_POINT);
    missingNative->RegisterRadio(mobility[1], 0, SaturatedRadioRole::STATION);
    diagnostic =
        CaptureFailure([&] { missingNative->CalcRxPower(txPowerDbm, mobility[0], mobility[1]); });
    NS_TEST_ASSERT_MSG_NE(diagnostic.find("native loss model"),
                          std::string::npos,
                          "Missing native model has wrong diagnostic: " << diagnostic);
}

/** Verify native RSSI solving and deterministic triangle/ring coordinates. */
class SaturatedTcpRssiPlacementTestCase : public TestCase
{
  public:
    /** Construct the RSSI-placement test. */
    SaturatedTcpRssiPlacementTestCase();

  private:
    void DoRun() override;
};

SaturatedTcpRssiPlacementTestCase::SaturatedTcpRssiPlacementTestCase()
    : TestCase("saturated TCP native RSSI placement")
{
}

void
SaturatedTcpRssiPlacementTestCase::DoRun()
{
    constexpr double txPowerDbm = 20.0;
    const std::array<double, 3> targets{-41.5, -50.0, -60.0};
    auto lossModel = CreateObject<LogDistancePropagationLossModel>();
    auto sender = CreateObject<ConstantPositionMobilityModel>();
    auto receiver = CreateObject<ConstantPositionMobilityModel>();
    sender->SetPosition(Vector(0.0, 0.0, 0.0));

    std::array<double, 3> distances;
    for (std::size_t i = 0; i < targets.size(); ++i)
    {
        distances[i] = SolveDistanceForRssi(lossModel, txPowerDbm, targets[i]);
        receiver->SetPosition(Vector(distances[i], 0.0, 0.0));
        const double received = lossModel->CalcRxPower(txPowerDbm, sender, receiver);
        NS_TEST_ASSERT_MSG_EQ_TOL(received,
                                  targets[i],
                                  0.5,
                                  "Solved distance does not reproduce native RSSI");
    }
    NS_TEST_ASSERT_MSG_GT(distances[1], distances[0], "Medium RSSI distance is not larger");
    NS_TEST_ASSERT_MSG_GT(distances[2], distances[1], "Low RSSI distance is not larger");

    constexpr double sideLength = 30.0;
    const auto triangle = BuildAccessPointTriangle(sideLength);
    NS_TEST_ASSERT_MSG_EQ_TOL(CalculateDistance(triangle[0], triangle[1]),
                              sideLength,
                              1e-12,
                              "First triangle side differs");
    NS_TEST_ASSERT_MSG_EQ_TOL(CalculateDistance(triangle[1], triangle[2]),
                              sideLength,
                              1e-12,
                              "Second triangle side differs");
    NS_TEST_ASSERT_MSG_EQ_TOL(CalculateDistance(triangle[2], triangle[0]),
                              sideLength,
                              1e-12,
                              "Third triangle side differs");

    constexpr uint32_t stationCount = 6;
    constexpr double offset = 0.17;
    constexpr double pi = 3.14159265358979323846;
    const auto ring = BuildStationRing(triangle[1], distances[1], stationCount, offset);
    NS_TEST_ASSERT_MSG_EQ(ring.size(), stationCount, "Wrong station ring size");
    std::array<double, stationCount> angles;
    for (std::size_t i = 0; i < ring.size(); ++i)
    {
        NS_TEST_ASSERT_MSG_EQ_TOL(CalculateDistance(triangle[1], ring[i]),
                                  distances[1],
                                  1e-9,
                                  "Station is not on the requested radius");
        angles[i] = std::atan2(ring[i].y - triangle[1].y, ring[i].x - triangle[1].x);
        if (i > 0 && angles[i] < angles[i - 1])
        {
            angles[i] += 2.0 * pi;
        }
        if (i > 0)
        {
            NS_TEST_ASSERT_MSG_EQ_TOL(angles[i] - angles[i - 1],
                                      2.0 * pi / stationCount,
                                      1e-12,
                                      "Station ring angles are not equally spaced");
        }
    }

    const auto offsetRing =
        BuildStationRing(triangle[1], distances[1], stationCount, offset + pi / stationCount);
    for (const auto& first : ring)
    {
        for (const auto& second : offsetRing)
        {
            NS_TEST_ASSERT_MSG_GT(CalculateDistance(first, second),
                                  1e-9,
                                  "BSS angular offsets produced identical coordinates");
        }
    }

    auto diagnostic =
        CaptureFailure([&] { SolveDistanceForRssi(nullptr, txPowerDbm, targets[0]); });
    NS_TEST_ASSERT_MSG_NE(diagnostic.find("loss model"),
                          std::string::npos,
                          "Null loss model has wrong diagnostic: " << diagnostic);
    auto constantLoss = CreateObject<ConstantLossModel>();
    diagnostic =
        CaptureFailure([&] { SolveDistanceForRssi(constantLoss, txPowerDbm, targets[0]); });
    NS_TEST_ASSERT_MSG_NE(diagnostic.find("bracket"),
                          std::string::npos,
                          "Impossible target has wrong diagnostic: " << diagnostic);
    auto increasingLoss = CreateObject<IncreasingLossModel>();
    diagnostic =
        CaptureFailure([&] { SolveDistanceForRssi(increasingLoss, txPowerDbm, targets[0]); });
    NS_TEST_ASSERT_MSG_NE(diagnostic.find("monotonic"),
                          std::string::npos,
                          "Increasing native model has wrong diagnostic: " << diagnostic);
}

/** Verify native-only chain ownership and exact stream delegation. */
class SaturatedTcpPropagationChainTestCase : public TestCase
{
  public:
    /** Construct the propagation-chain test. */
    SaturatedTcpPropagationChainTestCase();

  private:
    void DoRun() override;
};

SaturatedTcpPropagationChainTestCase::SaturatedTcpPropagationChainTestCase()
    : TestCase("saturated TCP native-only propagation chain")
{
}

void
SaturatedTcpPropagationChainTestCase::DoRun()
{
    auto accessPoint = CreateObject<ConstantPositionMobilityModel>();
    auto sameBssStation = CreateObject<ConstantPositionMobilityModel>();
    auto otherBssStation = CreateObject<ConstantPositionMobilityModel>();
    accessPoint->SetPosition(Vector(0.0, 0.0, 0.0));
    sameBssStation->SetPosition(Vector(1.0, 0.0, 0.0));
    otherBssStation->SetPosition(Vector(2.0, 0.0, 0.0));

    auto nativeHead = CreateObject<RecordingLossModel>();
    nativeHead->lossDb = 2.0;
    nativeHead->streamCount = 2;
    auto nativeTail = CreateObject<RecordingLossModel>();
    nativeTail->lossDb = 3.0;
    nativeTail->streamCount = 3;
    nativeHead->SetNext(nativeTail);

    auto filter = CreateObject<BssLinkFilterPropagationLossModel>();
    filter->SetNativeLossModel(nativeHead);
    filter->RegisterRadio(accessPoint, 0, SaturatedRadioRole::ACCESS_POINT);
    filter->RegisterRadio(sameBssStation, 0, SaturatedRadioRole::STATION);
    filter->RegisterRadio(otherBssStation, 1, SaturatedRadioRole::STATION);

    const double received = filter->CalcRxPower(20.0, accessPoint, sameBssStation);
    NS_TEST_ASSERT_MSG_EQ_TOL(received,
                              15.0,
                              1e-12,
                              "Native internal chain did not apply exactly once");
    NS_TEST_ASSERT_MSG_EQ(nativeHead->powerCalls, 1, "Native head power call count differs");
    NS_TEST_ASSERT_MSG_EQ(nativeTail->powerCalls, 1, "Native tail power call count differs");

    constexpr int64_t streamOffset = 41;
    NS_TEST_ASSERT_MSG_EQ(filter->AssignStreams(streamOffset),
                          5,
                          "Native chain stream count was not returned");
    NS_TEST_ASSERT_MSG_EQ(nativeHead->streamCalls, 1, "Native head stream call count differs");
    NS_TEST_ASSERT_MSG_EQ(nativeHead->lastStream,
                          streamOffset,
                          "Native head received the wrong stream offset");
    NS_TEST_ASSERT_MSG_EQ(nativeTail->streamCalls, 1, "Native tail stream call count differs");
    NS_TEST_ASSERT_MSG_EQ(nativeTail->lastStream,
                          streamOffset + nativeHead->streamCount,
                          "Native tail received the wrong stream offset");

    auto outer = CreateObject<FixedRssLossModel>();
    outer->SetRss(-12.0);
    filter->SetNext(outer);
    auto diagnostic =
        CaptureFailure([&] { filter->CalcRxPower(20.0, accessPoint, otherBssStation); });
    NS_TEST_ASSERT_MSG_NE(diagnostic.find("outer propagation loss chain"),
                          std::string::npos,
                          "Outer power chain was not rejected before use: " << diagnostic);
    diagnostic = CaptureFailure([&] { filter->CalcRxPower(20.0, accessPoint, sameBssStation); });
    NS_TEST_ASSERT_MSG_NE(diagnostic.find("outer propagation loss chain"),
                          std::string::npos,
                          "Outer allowed-link chain was not rejected before use: " << diagnostic);
    NS_TEST_ASSERT_MSG_EQ(nativeHead->powerCalls,
                          1,
                          "Rejected outer chain still delegated power calculation");
    diagnostic = CaptureFailure([&] { filter->AssignStreams(100); });
    NS_TEST_ASSERT_MSG_NE(diagnostic.find("outer propagation loss chain"),
                          std::string::npos,
                          "Outer stream chain was not rejected before use: " << diagnostic);
    NS_TEST_ASSERT_MSG_EQ(nativeHead->streamCalls,
                          1,
                          "Rejected outer chain still delegated stream assignment");

    auto missingNative = CreateObject<BssLinkFilterPropagationLossModel>();
    const auto missingPowerDiagnostic =
        CaptureFailure([&] { missingNative->CalcRxPower(20.0, accessPoint, sameBssStation); });
    const auto missingStreamDiagnostic =
        CaptureFailure([&] { missingNative->AssignStreams(streamOffset); });
    NS_TEST_ASSERT_MSG_NE(
        missingStreamDiagnostic.find("native loss model"),
        std::string::npos,
        "Missing native stream model has wrong diagnostic: " << missingStreamDiagnostic);
    NS_TEST_ASSERT_MSG_EQ(missingStreamDiagnostic,
                          missingPowerDiagnostic,
                          "Power and stream missing-native diagnostics differ");

    auto directCycle = CreateObject<BssLinkFilterPropagationLossModel>();
    diagnostic = CaptureFailure([&] { directCycle->SetNativeLossModel(directCycle); });
    NS_TEST_ASSERT_MSG_NE(diagnostic.find("cycle"),
                          std::string::npos,
                          "Direct native self-cycle was accepted: " << diagnostic);

    auto indirectCycle = CreateObject<BssLinkFilterPropagationLossModel>();
    auto cycleHead = CreateObject<RecordingLossModel>();
    cycleHead->SetNext(indirectCycle);
    diagnostic = CaptureFailure([&] { indirectCycle->SetNativeLossModel(cycleHead); });
    NS_TEST_ASSERT_MSG_NE(diagnostic.find("cycle"),
                          std::string::npos,
                          "Indirect native self-cycle was accepted: " << diagnostic);
}

} // namespace

std::vector<TestCase*>
CreateSaturatedTcpPropagationTestCases()
{
    return {
        new SaturatedTcpLinkPolicyTestCase(),
        new SaturatedTcpRssiPlacementTestCase(),
        new SaturatedTcpPropagationChainTestCase(),
    };
}
