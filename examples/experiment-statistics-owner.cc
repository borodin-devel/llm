#include "experiment-statistics-internal.h"
#include "experiment-statistics.h"
#include "traffic-coordinator.h"

#include "ns3/abort.h"
#include "ns3/wifi-net-device.h"

#include <sstream>
#include <utility>

namespace ns3
{

namespace
{

std::string
Ipv4ToString(Ipv4Address address)
{
    std::ostringstream stream;
    address.Print(stream);
    return stream.str();
}

} // namespace

ExperimentStatistics::ExperimentStatistics(const TrafficCoordinator& coordinator, uint32_t windowMs)
    : m_state(std::make_unique<ExperimentStatisticsState>(coordinator, windowMs))
{
}

ExperimentStatistics::~ExperimentStatistics() = default;

void
ExperimentStatistics::RegisterAccessPointIdentity(uint32_t accessPointId,
                                                  uint32_t nodeId,
                                                  std::string nodeLabel,
                                                  Ipv4Address ipv4)
{
    m_state->entityRegistry.RegisterAccessPoint(accessPointId,
                                                nodeId,
                                                std::move(nodeLabel),
                                                Ipv4ToString(ipv4));
}

void
ExperimentStatistics::RegisterStationIdentity(uint32_t accessPointId,
                                              uint32_t stationIndex,
                                              uint32_t nodeId,
                                              std::string nodeLabel,
                                              Ipv4Address ipv4)
{
    m_state->entityRegistry.RegisterStation(accessPointId,
                                            stationIndex,
                                            nodeId,
                                            std::move(nodeLabel),
                                            Ipv4ToString(ipv4));
}

void
ExperimentStatistics::RegisterWifiDevice(uint32_t nodeId, Ptr<NetDevice> device)
{
    Ptr<WifiNetDevice> wifi = DynamicCast<WifiNetDevice>(device);
    NS_ABORT_MSG_IF(!wifi, "Observability target is not a WifiNetDevice");
    ConnectPhyTraces(*m_state, nodeId, wifi);
    ConnectMacTraces(*m_state, nodeId, wifi);
}

void
ExperimentStatistics::Finalize()
{
    FinalizeDeviceStatistics(*m_state);
    FinalizeTcpStatistics();
}

} // namespace ns3
