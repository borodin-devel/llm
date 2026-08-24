#ifndef WIFI_STATISTICS_H
#define WIFI_STATISTICS_H

#include "ns3/address.h"
#include "ns3/ipv4-address.h"
#include "ns3/ptr.h"

#include <cstdint>
#include <memory>
#include <string>

namespace ns3
{

class APGenerator;
class Ipv4InterfaceContainer;
class NetDevice;
class StaLlmGenerator;
class TrafficCoordinator;
struct WifiStatisticsState;

/** Accumulate PHY rates weighted by allocated airtime. */
struct PhyRateAccumulator
{
    uint64_t txAttempts{0};              ///< Number of tagged transmit attempts.
    long double weightedRateBpsUs{0.0};  ///< Sum of rate multiplied by airtime.
    long double airtimeUs{0.0};          ///< Allocated airtime in microseconds.

    /**
     * Add one rate sample.
     *
     * @param rateBps PHY rate in bits per second.
     * @param allocatedAirtimeUs Allocated airtime in microseconds.
     */
    void Add(double rateBps, double allocatedAirtimeUs);

    /** @param other Accumulator to merge. */
    void Merge(const PhyRateAccumulator& other);

    /** @return Airtime-weighted rate in megabits per second. */
    double AverageMbps() const;

    /** @return Allocated airtime in microseconds. */
    double AirtimeUs() const;
};

/**
 * Resolve an absolute timestamp to a fixed statistics window.
 *
 * @param absoluteUs Absolute timestamp in microseconds.
 * @param experimentStartUs Common trace epoch in microseconds.
 * @param maxExperimentDurationMs Maximum experiment duration in milliseconds.
 * @param windowMs Window width in milliseconds.
 * @param windowIndex Resolved zero-based window index.
 * @return True when the timestamp lies inside the experiment interval.
 */
bool GetStatisticsWindowIndex(int64_t absoluteUs,
                              int64_t experimentStartUs,
                              double maxExperimentDurationMs,
                              uint32_t windowMs,
                              uint32_t& windowIndex);

/** Own all Wi-Fi trace, aggregation, and serialization state for one scenario. */
class WifiStatistics
{
  public:
    /** @param coordinator Traffic epoch and duration owner. */
    explicit WifiStatistics(const TrafficCoordinator& coordinator);
    ~WifiStatistics();

    void RegisterApGroup(int bssIndex,
                         Ipv4Address apAddress,
                         const Ipv4InterfaceContainer& stationInterfaces);
    void RegisterWifiDevice(uint32_t nodeId, std::string nodeLabel, Ptr<NetDevice> device);
    void ConnectApGenerator(Ptr<APGenerator> generator, uint32_t nodeId);
    void ConnectStaGenerator(Ptr<StaLlmGenerator> generator, uint32_t nodeId);
    void RecordMacPayload(int64_t nowUs,
                          const std::string& sourceIp,
                          const std::string& destinationIp,
                          uint32_t payloadBytes);
    void WriteJson(const std::string& outputPath) const;
    void PrintCrossLayerReport() const;

  private:
    std::unique_ptr<WifiStatisticsState> m_state; ///< Scenario statistics state.
};

} // namespace ns3

#endif // WIFI_STATISTICS_H
