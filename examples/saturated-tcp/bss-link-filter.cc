#include "bss-link-filter.h"

#include "ns3/mobility-model.h"
#include "ns3/node.h"

#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>

namespace ns3
{

namespace
{

/**
 * Describe a mobility model with all available identity information.
 *
 * @param mobility Mobility model to describe.
 * @return Mobility pointer and, when available, node identifier.
 */
std::string
DescribeMobility(Ptr<MobilityModel> mobility)
{
    std::ostringstream description;
    description << "mobility " << PeekPointer(mobility);
    if (mobility)
    {
        const auto node = mobility->GetObject<Node>();
        if (node)
        {
            description << " on node " << node->GetId();
        }
    }
    return description.str();
}

} // namespace

NS_OBJECT_ENSURE_REGISTERED(BssLinkFilterPropagationLossModel);

TypeId
BssLinkFilterPropagationLossModel::GetTypeId()
{
    static TypeId tid = TypeId("ns3::BssLinkFilterPropagationLossModel")
                            .SetParent<PropagationLossModel>()
                            .SetGroupName("Llm")
                            .AddConstructor<BssLinkFilterPropagationLossModel>();
    return tid;
}

void
BssLinkFilterPropagationLossModel::SetNativeLossModel(Ptr<PropagationLossModel> nativeLoss)
{
    if (!nativeLoss)
    {
        throw std::invalid_argument("native loss model must not be null");
    }
    m_nativeLoss = nativeLoss;
}

void
BssLinkFilterPropagationLossModel::RegisterRadio(Ptr<MobilityModel> mobility,
                                                 uint32_t bssId,
                                                 SaturatedRadioRole role)
{
    if (!mobility)
    {
        throw std::invalid_argument("cannot register null mobility");
    }
    const auto [iterator, inserted] = m_radios.emplace(mobility, RadioIdentity{bssId, role});
    if (!inserted)
    {
        throw std::invalid_argument(DescribeMobility(iterator->first) + " is already registered");
    }
}

double
BssLinkFilterPropagationLossModel::DoCalcRxPower(double txPowerDbm,
                                                 Ptr<MobilityModel> sender,
                                                 Ptr<MobilityModel> receiver) const
{
    if (!m_nativeLoss)
    {
        throw std::runtime_error("BSS link filter has no native loss model");
    }

    const auto senderIdentity = m_radios.find(sender);
    if (senderIdentity == m_radios.end())
    {
        throw std::runtime_error("unregistered sender " + DescribeMobility(sender));
    }
    const auto receiverIdentity = m_radios.find(receiver);
    if (receiverIdentity == m_radios.end())
    {
        throw std::runtime_error("unregistered receiver " + DescribeMobility(receiver));
    }

    const bool sameBss = senderIdentity->second.bssId == receiverIdentity->second.bssId;
    const bool bothAccessPoints = senderIdentity->second.role == SaturatedRadioRole::ACCESS_POINT &&
                                  receiverIdentity->second.role == SaturatedRadioRole::ACCESS_POINT;
    if (!sameBss && !bothAccessPoints)
    {
        return -std::numeric_limits<double>::infinity();
    }
    return m_nativeLoss->CalcRxPower(txPowerDbm, sender, receiver);
}

int64_t
BssLinkFilterPropagationLossModel::DoAssignStreams(int64_t stream)
{
    return m_nativeLoss ? m_nativeLoss->AssignStreams(stream) : 0;
}

} // namespace ns3
