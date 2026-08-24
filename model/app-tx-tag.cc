#include "app-tx-tag.h"

#include <utility>

namespace ns3
{

AppTxTag::AppTxTag(uint64_t appPacketUid,
                   int64_t appTxTimeUs,
                   Ipv4Address source,
                   Ipv4Address destination,
                   uint16_t sourcePort,
                   uint16_t destinationPort,
                   uint32_t appPayloadBytes,
                   std::string agentKey)
    : m_appPacketUid(appPacketUid),
      m_appTxTimeUs(appTxTimeUs),
      m_sourceIpv4(source.Get()),
      m_destinationIpv4(destination.Get()),
      m_sourcePort(sourcePort),
      m_destinationPort(destinationPort),
      m_appPayloadBytes(appPayloadBytes),
      m_agentKey(std::move(agentKey))
{
}

TypeId
AppTxTag::GetTypeId()
{
    static TypeId tid = TypeId("ns3::AppTxTag").SetParent<Tag>().AddConstructor<AppTxTag>();
    return tid;
}

TypeId
AppTxTag::GetInstanceTypeId() const
{
    return GetTypeId();
}

uint32_t
AppTxTag::GetSerializedSize() const
{
    return 8 + 8 + 4 + 4 + 2 + 2 + 4 + 4 + static_cast<uint32_t>(m_agentKey.size());
}

void
AppTxTag::Serialize(TagBuffer buffer) const
{
    buffer.WriteU64(m_appPacketUid);
    buffer.WriteU64(static_cast<uint64_t>(m_appTxTimeUs));
    buffer.WriteU32(m_sourceIpv4);
    buffer.WriteU32(m_destinationIpv4);
    buffer.WriteU16(m_sourcePort);
    buffer.WriteU16(m_destinationPort);
    buffer.WriteU32(m_appPayloadBytes);
    const auto keySize = static_cast<uint32_t>(m_agentKey.size());
    buffer.WriteU32(keySize);
    if (keySize > 0)
    {
        buffer.Write(reinterpret_cast<const uint8_t*>(m_agentKey.data()), keySize);
    }
}

void
AppTxTag::Deserialize(TagBuffer buffer)
{
    m_appPacketUid = buffer.ReadU64();
    m_appTxTimeUs = static_cast<int64_t>(buffer.ReadU64());
    m_sourceIpv4 = buffer.ReadU32();
    m_destinationIpv4 = buffer.ReadU32();
    m_sourcePort = buffer.ReadU16();
    m_destinationPort = buffer.ReadU16();
    m_appPayloadBytes = buffer.ReadU32();
    const uint32_t keySize = buffer.ReadU32();
    m_agentKey.resize(keySize);
    if (keySize > 0)
    {
        buffer.Read(reinterpret_cast<uint8_t*>(m_agentKey.data()), keySize);
    }
}

void
AppTxTag::Print(std::ostream& os) const
{
    os << "uid=" << m_appPacketUid << " appTxUs=" << m_appTxTimeUs
       << " src=" << Ipv4Address(m_sourceIpv4) << ":" << m_sourcePort
       << " dst=" << Ipv4Address(m_destinationIpv4) << ":" << m_destinationPort
       << " payload=" << m_appPayloadBytes << " agent=\"" << m_agentKey << "\"";
}

uint64_t
AppTxTag::GetAppPacketUid() const
{
    return m_appPacketUid;
}

int64_t
AppTxTag::GetAppTxTimeUs() const
{
    return m_appTxTimeUs;
}

Ipv4Address
AppTxTag::GetSource() const
{
    return Ipv4Address(m_sourceIpv4);
}

Ipv4Address
AppTxTag::GetDestination() const
{
    return Ipv4Address(m_destinationIpv4);
}

uint16_t
AppTxTag::GetSourcePort() const
{
    return m_sourcePort;
}

uint16_t
AppTxTag::GetDestinationPort() const
{
    return m_destinationPort;
}

uint32_t
AppTxTag::GetAppPayloadBytes() const
{
    return m_appPayloadBytes;
}

const std::string&
AppTxTag::GetAgentKey() const
{
    return m_agentKey;
}

void
AddAppTxTag(Ptr<Packet> packet,
            Time txTime,
            const InetSocketAddress& source,
            const InetSocketAddress& destination,
            const std::string& agentKey)
{
    AppTxTag tag(packet->GetUid(),
                 txTime.GetMicroSeconds(),
                 source.GetIpv4(),
                 destination.GetIpv4(),
                 source.GetPort(),
                 destination.GetPort(),
                 packet->GetSize(),
                 agentKey);
    packet->AddByteTag(tag);
}

} // namespace ns3
