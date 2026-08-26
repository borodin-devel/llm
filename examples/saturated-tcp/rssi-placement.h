#ifndef SATURATED_TCP_RSSI_PLACEMENT_H
#define SATURATED_TCP_RSSI_PLACEMENT_H

#include "ns3/ptr.h"
#include "ns3/vector.h"

#include <array>
#include <cstdint>
#include <vector>

namespace ns3
{

class PropagationLossModel;

/**
 * Solve a positive separation that produces a target native RSSI.
 *
 * @param lossModel Concrete native propagation loss model.
 * @param txPowerDbm Transmit power in dBm.
 * @param targetRssiDbm Target receive power in dBm.
 * @return Positive distance in meters.
 * @throws std::runtime_error if no monotonic solution can be bracketed.
 */
double SolveDistanceForRssi(Ptr<PropagationLossModel> lossModel,
                            double txPowerDbm,
                            double targetRssiDbm);

/**
 * Build an equilateral access-point triangle in the XY plane.
 *
 * @param sideLengthM Triangle side length in meters.
 * @return Three access-point coordinates.
 */
std::array<Vector, 3> BuildAccessPointTriangle(double sideLengthM);

/**
 * Build equally spaced station coordinates around an access point.
 *
 * @param accessPointPosition Ring center.
 * @param radiusM Ring radius in meters.
 * @param stationCount Number of station coordinates.
 * @param angularOffsetRadians Angular offset of the first station.
 * @return Station coordinates in index order.
 */
std::vector<Vector> BuildStationRing(const Vector& accessPointPosition,
                                     double radiusM,
                                     uint32_t stationCount,
                                     double angularOffsetRadians);

} // namespace ns3

#endif // SATURATED_TCP_RSSI_PLACEMENT_H
