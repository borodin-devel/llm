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
     * @param source Source IPv4 address.
     * @param destination Destination IPv4 address.
     * @param sourcePort Source TCP port.
     * @param destinationPort Destination TCP port.
     * @param appPayloadBytes Application payload size in bytes.
     * @param agentKey Application-level agent identifier.
     */
    AppTxTag(uint64_t appPacketUid,
             int64_t appTxTimeUs,
             Ipv4Address source,
             Ipv4Address destination,
             uint16_t sourcePort,
             uint16_t destinationPort,
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

    /** @return Application packet UID. */
    uint64_t GetAppPacketUid() const;

    /** @return Application transmit time in microseconds. */
    int64_t GetAppTxTimeUs() const;

    /** @return Source IPv4 address. */
    Ipv4Address GetSource() const;

    /** @return Destination IPv4 address. */
    Ipv4Address GetDestination() const;

    /** @return Source TCP port. */
    uint16_t GetSourcePort() const;

    /** @return Destination TCP port. */
    uint16_t GetDestinationPort() const;

    /** @return Application payload size in bytes. */
    uint32_t GetAppPayloadBytes() const;

    /** @return Application-level agent identifier. */
    const std::string& GetAgentKey() const;

  private:
    uint64_t m_appPacketUid{0};    ///< Application packet UID.
    int64_t m_appTxTimeUs{0};      ///< Application transmit time in microseconds.
    uint32_t m_sourceIpv4{0};      ///< Serialized source IPv4 address.
    uint32_t m_destinationIpv4{0}; ///< Serialized destination IPv4 address.
    uint16_t m_sourcePort{0};      ///< Source TCP port.
    uint16_t m_destinationPort{0}; ///< Destination TCP port.
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
