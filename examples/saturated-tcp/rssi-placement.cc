#include "rssi-placement.h"

#include "ns3/constant-position-mobility-model.h"
#include "ns3/propagation-loss-model.h"

#include <cmath>
#include <stdexcept>

namespace ns3
{

namespace
{

constexpr double RSSI_TOLERANCE_DB = 0.01;
constexpr double MONOTONIC_TOLERANCE_DB = 1e-9;
constexpr double MINIMUM_DISTANCE_M = 1e-9;
constexpr uint32_t MAX_BRACKET_ITERATIONS = 128;
constexpr uint32_t MAX_SOLVE_ITERATIONS = 256;
constexpr double PI = 3.14159265358979323846;

/**
 * Evaluate one concrete native propagation model at a separation.
 *
 * @param lossModel Native loss model.
 * @param sender Fixed sender mobility.
 * @param receiver Fixed receiver mobility.
 * @param txPowerDbm Transmit power in dBm.
 * @param distanceM Separation in meters.
 * @return Receive power in dBm.
 */
double
EvaluateRssi(Ptr<PropagationLossModel> lossModel,
             Ptr<ConstantPositionMobilityModel> sender,
             Ptr<ConstantPositionMobilityModel> receiver,
             double txPowerDbm,
             double distanceM)
{
    receiver->SetPosition(Vector(distanceM, 0.0, 0.0));
    const double received = lossModel->CalcRxPower(txPowerDbm, sender, receiver);
    if (!std::isfinite(received))
    {
        throw std::runtime_error("native loss model returned a non-finite RSSI");
    }
    return received;
}

} // namespace

double
SolveDistanceForRssi(Ptr<PropagationLossModel> lossModel, double txPowerDbm, double targetRssiDbm)
{
    if (!lossModel)
    {
        throw std::invalid_argument("RSSI distance solver requires a loss model");
    }
    if (!std::isfinite(txPowerDbm) || !std::isfinite(targetRssiDbm))
    {
        throw std::invalid_argument("RSSI distance solver requires finite powers");
    }

    auto sender = CreateObject<ConstantPositionMobilityModel>();
    auto receiver = CreateObject<ConstantPositionMobilityModel>();
    sender->SetPosition(Vector(0.0, 0.0, 0.0));

    double lowerDistance = MINIMUM_DISTANCE_M;
    double lowerRssi = EvaluateRssi(lossModel, sender, receiver, txPowerDbm, lowerDistance);
    if (std::abs(lowerRssi - targetRssiDbm) <= RSSI_TOLERANCE_DB)
    {
        return lowerDistance;
    }
    if (lowerRssi < targetRssiDbm)
    {
        throw std::runtime_error("cannot bracket target RSSI above the native model maximum");
    }

    double upperDistance = 1.0;
    double upperRssi = lowerRssi;
    bool bracketed = false;
    for (uint32_t iteration = 0; iteration < MAX_BRACKET_ITERATIONS; ++iteration)
    {
        upperRssi = EvaluateRssi(lossModel, sender, receiver, txPowerDbm, upperDistance);
        if (upperRssi > lowerRssi + MONOTONIC_TOLERANCE_DB)
        {
            throw std::runtime_error("native RSSI is not monotonic with distance");
        }
        if (std::abs(upperRssi - targetRssiDbm) <= RSSI_TOLERANCE_DB)
        {
            return upperDistance;
        }
        if (upperRssi < targetRssiDbm)
        {
            bracketed = true;
            break;
        }
        lowerDistance = upperDistance;
        lowerRssi = upperRssi;
        upperDistance *= 2.0;
    }
    if (!bracketed)
    {
        throw std::runtime_error("cannot bracket target RSSI with the native loss model");
    }

    for (uint32_t iteration = 0; iteration < MAX_SOLVE_ITERATIONS; ++iteration)
    {
        const double middleDistance = (lowerDistance + upperDistance) / 2.0;
        const double middleRssi =
            EvaluateRssi(lossModel, sender, receiver, txPowerDbm, middleDistance);
        if (middleRssi > lowerRssi + MONOTONIC_TOLERANCE_DB ||
            middleRssi < upperRssi - MONOTONIC_TOLERANCE_DB)
        {
            throw std::runtime_error("native RSSI is not monotonic with distance");
        }
        if (std::abs(middleRssi - targetRssiDbm) <= RSSI_TOLERANCE_DB)
        {
            return middleDistance;
        }
        if (middleRssi > targetRssiDbm)
        {
            lowerDistance = middleDistance;
            lowerRssi = middleRssi;
        }
        else
        {
            upperDistance = middleDistance;
            upperRssi = middleRssi;
        }
    }
    throw std::runtime_error("native RSSI has no monotonic solution within tolerance");
}

std::array<Vector, 3>
BuildAccessPointTriangle(double sideLengthM)
{
    return {
        Vector(0.0, 0.0, 0.0),
        Vector(sideLengthM, 0.0, 0.0),
        Vector(sideLengthM / 2.0, std::sqrt(3.0) * sideLengthM / 2.0, 0.0),
    };
}

std::vector<Vector>
BuildStationRing(const Vector& accessPointPosition,
                 double radiusM,
                 uint32_t stationCount,
                 double angularOffsetRadians)
{
    std::vector<Vector> positions;
    positions.reserve(stationCount);
    for (uint32_t index = 0; index < stationCount; ++index)
    {
        const double angle =
            2.0 * PI * static_cast<double>(index) / stationCount + angularOffsetRadians;
        positions.emplace_back(accessPointPosition.x + radiusM * std::cos(angle),
                               accessPointPosition.y + radiusM * std::sin(angle),
                               accessPointPosition.z);
    }
    return positions;
}

} // namespace ns3
