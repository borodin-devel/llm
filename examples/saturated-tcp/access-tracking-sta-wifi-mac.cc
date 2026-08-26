#include "access-tracking-sta-wifi-mac.h"

#include "ns3/qos-txop.h"

namespace ns3
{

NS_OBJECT_ENSURE_REGISTERED(AccessTrackingStaWifiMac);

TypeId
AccessTrackingStaWifiMac::GetTypeId()
{
    static TypeId tid =
        TypeId("ns3::AccessTrackingStaWifiMac")
            .SetParent<StaWifiMac>()
            .SetGroupName("Llm")
            .AddConstructor<AccessTrackingStaWifiMac>()
            .AddTraceSource("AccessRequested",
                            "A station EDCA or DCF has requested channel access",
                            MakeTraceSourceAccessor(&AccessTrackingStaWifiMac::m_accessRequested),
                            "ns3::AccessTrackingStaWifiMac::AccessRequestedCallback");
    return tid;
}

std::vector<AccessGrantStartNs>
AccessTrackingStaWifiMac::GetActiveTxopStartTimes() const
{
    std::vector<AccessGrantStartNs> starts;
    for (const auto ac : {AC_BE, AC_BK, AC_VI, AC_VO})
    {
        const auto txop = GetQosTxop(ac);
        if (!txop)
        {
            continue;
        }
        for (const auto linkId : GetLinkIds())
        {
            if (const auto start = txop->GetTxopStartTime(linkId))
            {
                starts.push_back({ac, linkId, start->GetNanoSeconds()});
            }
        }
    }
    return starts;
}

void
AccessTrackingStaWifiMac::NotifyRequestAccess(Ptr<Txop> txop, uint8_t linkId)
{
    uint8_t ac = AC_BE_NQOS;
    if (const auto qosTxop = DynamicCast<QosTxop>(txop))
    {
        ac = qosTxop->GetAccessCategory();
    }
    m_accessRequested(ac, linkId);
    StaWifiMac::NotifyRequestAccess(txop, linkId);
}

} // namespace ns3
