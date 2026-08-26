#ifndef SATURATED_TCP_BSS_LINK_FILTER_H
#define SATURATED_TCP_BSS_LINK_FILTER_H

#include "ns3/propagation-loss-model.h"

#include <cstdint>
#include <map>

namespace ns3
{

/** Radio roles used by the saturated benchmark propagation policy. */
enum class SaturatedRadioRole
{
    ACCESS_POINT, ///< Wi-Fi access point.
    STATION,      ///< Wi-Fi station.
};

/**
 * Apply the saturated benchmark BSS policy around a native loss model.
 *
 * Links within one BSS always use the supplied native model. Links between
 * BSSs use the native model only when both endpoints are access points.
 */
class BssLinkFilterPropagationLossModel : public PropagationLossModel
{
  public:
    /**
     * Get the object TypeId.
     *
     * @return The object TypeId.
     */
    static TypeId GetTypeId();

    /**
     * Set the native model used for allowed links.
     *
     * @param nativeLoss Native propagation loss model.
     * @throws std::invalid_argument if @p nativeLoss is null.
     */
    void SetNativeLossModel(Ptr<PropagationLossModel> nativeLoss);

    /**
     * Register one radio identity.
     *
     * @param mobility Radio mobility model.
     * @param bssId BSS identifier.
     * @param role Radio role.
     * @throws std::invalid_argument if @p mobility is null or already registered.
     */
    void RegisterRadio(Ptr<MobilityModel> mobility, uint32_t bssId, SaturatedRadioRole role);

  private:
    /** Registered policy identity for one radio. */
    struct RadioIdentity
    {
        uint32_t bssId;          ///< BSS identifier.
        SaturatedRadioRole role; ///< Radio role.
    };

    double DoCalcRxPower(double txPowerDbm,
                         Ptr<MobilityModel> sender,
                         Ptr<MobilityModel> receiver) const override;
    int64_t DoAssignStreams(int64_t stream) override;

    Ptr<PropagationLossModel> m_nativeLoss;               ///< Native model for allowed links.
    std::map<Ptr<MobilityModel>, RadioIdentity> m_radios; ///< Registered radio identities.
};

} // namespace ns3

#endif // SATURATED_TCP_BSS_LINK_FILTER_H
