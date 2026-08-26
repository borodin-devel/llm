#ifndef SATURATED_TCP_ACCESS_TRACKING_STA_WIFI_MAC_H
#define SATURATED_TCP_ACCESS_TRACKING_STA_WIFI_MAC_H

#include "access-wait-tracker.h"

#include "ns3/sta-wifi-mac.h"
#include "ns3/traced-callback.h"

#include <cstdint>
#include <vector>

namespace ns3
{

/** Station MAC that reports requests for channel access without changing MAC policy. */
class AccessTrackingStaWifiMac : public StaWifiMac
{
  public:
    /**
     * Get the object TypeId.
     *
     * @return The object TypeId.
     */
    static TypeId GetTypeId();

    /**
     * Get historical starts for all QoS TXOPs active on current links.
     *
     * @return Active access-category and link grant starts in nanoseconds.
     */
    std::vector<AccessGrantStartNs> GetActiveTxopStartTimes() const;

    /**
     * Access-request trace callback signature.
     *
     * @param ac Access category.
     * @param linkId Link identifier.
     */
    using AccessRequestedCallback = void (*)(uint8_t ac, uint8_t linkId);

  protected:
    void NotifyRequestAccess(Ptr<Txop> txop, uint8_t linkId) override;

  private:
    TracedCallback<uint8_t, uint8_t> m_accessRequested; ///< Access-request trace source.
};

} // namespace ns3

#endif // SATURATED_TCP_ACCESS_TRACKING_STA_WIFI_MAC_H
