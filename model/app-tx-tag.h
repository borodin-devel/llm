#ifndef APP_TX_TAG_H
#define APP_TX_TAG_H

#include "ns3/inet-socket-address.h"
#include "ns3/ipv4-address.h"
#include "ns3/nstime.h"
#include "ns3/packet.h"
#include "ns3/tag.h"

#include <cstdint>
#include <ostream>
#include <string>

namespace ns3
{

/**
 * Byte tag that carries application transmit metadata to the Wi-Fi PHY.
 *
 * A byte tag is required because TCP may split or merge application writes.
 * The metadata follows the tagged payload bytes through those transformations.
 */
class AppTxTag : public Tag
{
  public:
    AppTxTag() = default;

    /**
     * Construct application transmit metadata.
     *
     * @param appPacketUid Application packet UID.
     * @param appTxTimeUs Application transmit time in microseconds.
     * @param src Source IPv4 address.
     * @param dst Destination IPv4 address.
     * @param srcPort Source TCP port.
     * @param dstPort Destination TCP port.
     * @param appPayloadBytes Application payload size in bytes.
     * @param agentKey Application-level agent identifier.
     */
    AppTxTag(uint64_t appPacketUid,
             int64_t appTxTimeUs,
             Ipv4Address src,
             Ipv4Address dst,
             uint16_t srcPort,
             uint16_t dstPort,
             uint32_t appPayloadBytes,
             std::string agentKey);

    /**
     * Get the registered type identifier.
     *
     * @return Tag TypeId.
     */
    static TypeId GetTypeId();

    TypeId GetInstanceTypeId() const override;
    uint32_t GetSerializedSize() const override;
    void Serialize(TagBuffer buffer) const override;
    void Deserialize(TagBuffer buffer) override;
    void Print(std::ostream& os) const override;

    uint64_t GetAppPacketUid() const;
    int64_t GetAppTxTimeUs() const;
    Ipv4Address GetSource() const;
    Ipv4Address GetDestination() const;
    uint16_t GetSourcePort() const;
    uint16_t GetDestinationPort() const;
    uint32_t GetAppPayloadBytes() const;
    const std::string& GetAgentKey() const;

  private:
    uint64_t m_appPacketUid{0};     ///< Application packet UID.
    int64_t m_appTxTimeUs{0};      ///< Application transmit time in microseconds.
    uint32_t m_srcIpv4{0};         ///< Serialized source IPv4 address.
    uint32_t m_dstIpv4{0};         ///< Serialized destination IPv4 address.
    uint16_t m_srcPort{0};         ///< Source TCP port.
    uint16_t m_dstPort{0};         ///< Destination TCP port.
    uint32_t m_appPayloadBytes{0}; ///< Application payload size in bytes.
    std::string m_agentKey;        ///< Application-level agent identifier.
};

/**
 * Attach application transmit metadata to all payload bytes in a packet.
 *
 * @param packet Application payload packet.
 * @param txTime Application transmit time.
 * @param source Source socket address.
 * @param destination Destination socket address.
 * @param agentKey Application-level agent identifier.
 */
void AddAppTxTag(Ptr<Packet> packet,
                 Time txTime,
                 const InetSocketAddress& source,
                 const InetSocketAddress& destination,
                 const std::string& agentKey);

} // namespace ns3

#endif // APP_TX_TAG_H
