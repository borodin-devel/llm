#include "wifi-statistics.h"

#include "wifi-statistics-internal.h"

#include "ns3/abort.h"
#include "ns3/wifi-net-device.h"

#include <string>

namespace ns3
{

bool
GetNodeSecondIndex(int64_t relativeUs, int64_t experimentDurationUs, uint64_t& secondIndex)
{
    if (relativeUs < 0 || relativeUs >= experimentDurationUs)
    {
        return false;
    }

    secondIndex = static_cast<uint64_t>(relativeUs / 1000000);
    return true;
}

namespace
{

void
RegisterWifiObservability(WifiStatisticsState& statistics,
                          uint32_t nodeId,
                          const std::string& label,
                          Ptr<NetDevice> device)
{
    Ptr<WifiNetDevice> wifi = DynamicCast<WifiNetDevice>(device);
    NS_ABORT_MSG_IF(!wifi, "Observability target is not a WifiNetDevice");

    statistics.nodeLabels[nodeId] = label;
    ConnectPhyTraces(statistics, nodeId, wifi);
    ConnectMacTraces(statistics, nodeId, wifi);
}

} // namespace

void
WifiStatistics::RegisterWifiDevice(uint32_t nodeId, std::string nodeLabel, Ptr<NetDevice> device)
{
    RegisterWifiObservability(*m_state, nodeId, nodeLabel, device);
}

} // namespace ns3
