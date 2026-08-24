// model/agent-distribution.h
//
// Agent Distribution Module
// Greedy AP assignment (50ms slots) + simple station assignment
// Input: parsed agent data from JSON
// Output: 3 AP groups, each with agent maps and station assignments
//

#ifndef AGENT_DISTRIBUTION_H
#define AGENT_DISTRIBUTION_H

#include <string>
#include <vector>
#include <map>
#include <utility>
#include <numeric>
#include <cstdint>
#include <ostream>

#include "ns3/ipv4-address.h"
#include "ns3/tag.h"
#include "ns3/tag-buffer.h"
#include "ns3/type-id.h"

namespace ns3
{

class Address;

// ============================================================================
// Cross-layer application -> PHY trace metadata
// ============================================================================

/**
 * Byte tag attached to application payload before Socket::Send().
 *
 * ByteTag is used deliberately instead of PacketTag: TCP may split or merge
 * application writes, while byte tags keep following the tagged payload bytes.
 * At WifiPhy::PhyTxBegin the tag therefore identifies which bytes originated
 * from which application send event without guessing protocol header sizes.
 */
class AppTxTag : public Tag
{
  public:
    AppTxTag() = default;

    AppTxTag(uint64_t appPacketUid,
             int64_t appTxTimeUs,
             Ipv4Address src,
             Ipv4Address dst,
             uint16_t srcPort,
             uint16_t dstPort,
             uint32_t appPayloadBytes,
             std::string agentKey)
        : m_appPacketUid(appPacketUid),
          m_appTxTimeUs(appTxTimeUs),
          m_srcIpv4(src.Get()),
          m_dstIpv4(dst.Get()),
          m_srcPort(srcPort),
          m_dstPort(dstPort),
          m_appPayloadBytes(appPayloadBytes),
          m_agentKey(std::move(agentKey))
    {
    }

    static TypeId GetTypeId()
    {
        static TypeId tid = TypeId("ns3::AppTxTag")
                                .SetParent<Tag>()
                                .AddConstructor<AppTxTag>();
        return tid;
    }

    TypeId GetInstanceTypeId() const override
    {
        return GetTypeId();
    }

    uint32_t GetSerializedSize() const override
    {
        return 8 + 8 + 4 + 4 + 2 + 2 + 4 + 4 +
               static_cast<uint32_t>(m_agentKey.size());
    }

    void Serialize(TagBuffer buffer) const override
    {
        buffer.WriteU64(m_appPacketUid);
        buffer.WriteU64(static_cast<uint64_t>(m_appTxTimeUs));
        buffer.WriteU32(m_srcIpv4);
        buffer.WriteU32(m_dstIpv4);
        buffer.WriteU16(m_srcPort);
        buffer.WriteU16(m_dstPort);
        buffer.WriteU32(m_appPayloadBytes);
        const auto keySize = static_cast<uint32_t>(m_agentKey.size());
        buffer.WriteU32(keySize);
        if (keySize > 0)
        {
            buffer.Write(reinterpret_cast<const uint8_t*>(m_agentKey.data()), keySize);
        }
    }

    void Deserialize(TagBuffer buffer) override
    {
        m_appPacketUid = buffer.ReadU64();
        m_appTxTimeUs = static_cast<int64_t>(buffer.ReadU64());
        m_srcIpv4 = buffer.ReadU32();
        m_dstIpv4 = buffer.ReadU32();
        m_srcPort = buffer.ReadU16();
        m_dstPort = buffer.ReadU16();
        m_appPayloadBytes = buffer.ReadU32();
        const uint32_t keySize = buffer.ReadU32();
        m_agentKey.resize(keySize);
        if (keySize > 0)
        {
            buffer.Read(reinterpret_cast<uint8_t*>(m_agentKey.data()), keySize);
        }
    }

    void Print(std::ostream& os) const override
    {
        os << "uid=" << m_appPacketUid
           << " appTxUs=" << m_appTxTimeUs
           << " src=" << Ipv4Address(m_srcIpv4) << ":" << m_srcPort
           << " dst=" << Ipv4Address(m_dstIpv4) << ":" << m_dstPort
           << " payload=" << m_appPayloadBytes
           << " agent=\"" << m_agentKey << "\"";
    }

    uint64_t GetAppPacketUid() const { return m_appPacketUid; }
    int64_t GetAppTxTimeUs() const { return m_appTxTimeUs; }
    Ipv4Address GetSource() const { return Ipv4Address(m_srcIpv4); }
    Ipv4Address GetDestination() const { return Ipv4Address(m_dstIpv4); }
    uint16_t GetSourcePort() const { return m_srcPort; }
    uint16_t GetDestinationPort() const { return m_dstPort; }
    uint32_t GetAppPayloadBytes() const { return m_appPayloadBytes; }
    const std::string& GetAgentKey() const { return m_agentKey; }

  private:
    uint64_t m_appPacketUid{0};
    int64_t m_appTxTimeUs{0};
    uint32_t m_srcIpv4{0};
    uint32_t m_dstIpv4{0};
    uint16_t m_srcPort{0};
    uint16_t m_dstPort{0};
    uint32_t m_appPayloadBytes{0};
    std::string m_agentKey;
};

// ============================================================================
// Parsing result (from json_parser API)
// ============================================================================

struct Operation
{
    int downlinkBytes;
    double startOffsetMs;
    double endMs;
    int uplinkBytes;
};

struct AgentInfo
{
    std::string key;  // "id_type" (e.g. "1_GUI交互综合Agent")
    int id;
    int type;
    std::vector<Operation> operations;
};

struct ParsedResult
{
    std::vector<AgentInfo> agents;

    // Maximum (startOffsetMs + durationMs) across every operation in the
    // source JSON, including local operations that do not generate traffic.
    double experimentDurationMs{0.0};
};

// ============================================================================
// Distribution result
// ============================================================================

struct DistributionResult
{
    // 3 APs, each: map<agentKey("id_type"), list of operations>
    std::vector<std::map<std::string, std::vector<Operation>>> apAgentMaps;
    // 3 APs, each: map<agentKey("id_type"), station address>
    std::vector<std::map<std::string, Address>> apStationMaps;
    // 3 AP addresses
    std::vector<Address> apAddresses;
    // 3 AP base addresses (stations connect here)
    std::vector<Address> stationBases;
};

// ============================================================================
// API
// ============================================================================

/**
 * @brief Parse JSON traces file into agent data.
 *
 * Reads the JSON structure from json_parser, assigns numeric types,
 * and returns a flat list of agents with their operations.
 *
 * @param jsonPath Path to the JSON file.
 * @return ParsedResult with all agents.
 */
ParsedResult
ParseJsonFile(const std::string& jsonPath);

/**
 * @brief Distribute agents across APs and then to stations.
 *
 * Phase 1 - AP assignment (greedy, 50ms slots):
 *   - Divide time into 50ms slots
 *   - For each agent, find max concurrent operations in any slot (peakLoad)
 *   - Sort agents by peakLoad (descending)
 *   - For each agent, assign to AP with minimum max-concurrent-stations
 *   - Tie-break: balance by total bytes
 *
 * Phase 2 - Station assignment (greedy, per-AP):
 *   - For each AP, assign agents to stations round-robin
 *   - Each station can have at most maxAgentsPerStation agents
 *   - Overlapping agents CAN share a station (TCP ordering, no collision)
 *   - Only constraint: maxAgentsPerStation limit
 *
 * @param parsed ParsedResult from ParseJsonFile.
 * @param nAp Number of APs (default 3).
 * @param nStationsPerAp Max stations per AP (default 30).
 * @param maxAgentsPerAp Max agents per station (default 3).
 * @return DistributionResult.
 */
// TODO: Original Idea: need to minimize concurrency inside BSS between stations. There are 3 BSS and X STA per each BSS, up to 3 'agents' (ParsedResult) possible to put per 1 STA. Each agent has array of operations: 1 operation is a {dlBytes, startMs, upBytes, endMs}. 'start' is a start for downlink, 'end' is a start for uplink. So better to minimize intra-BSS concurrency and firstly try to distribute concurrent 'agents' into different BSS (empirical assumption, recheck) and inside BSS it is better to put them on same STA. Agent - is application level, TCP is used, so 'concurrent' payload will be queued correctly within 1 sta - no conflicts. Also, these 'operations' have UL and DL traffic, but it seems unreal to reduce concurrency in both directions, so need to choose direction with higher payload size in total (empirical assumption, recheck). Constraints: 3 APs, X stations (from 1 to 30), up to Y=3 agents per station. Also, it is required to use as many stations as possible (for example, if there are 90 agents, then 1 agent per sta is required).
// Proposal: this proposal can be adjusted or provide better solution.
// 1. for each agent count DL, UL bytes and choose by which field to sort 'operations' (start or end).
// 2. for each agent fit operations into X=50ms window and compare concurrency inside each window for each agent. For example, first 50ms might have no inter-agent cocurrency, second 50ms has 4 inter-agent cocurrencies, third = 2 concurrencies. Concurrency here means at least 1 operation for >=2 agents within same time window, even if their actual 'start'/'end' doesn't match.
// 3. take window with max concurrency, take 1 agent (first, random, whatever) and put it into BSS-1, recompute concurrency windows for other agents, take window with max concurrency, take 1 agent (first, random, whatever) and put it into BSS-2, recompute concurrency windows for other agents, take window with max concurrency, take 1 agent (first, random, whatever) and put it into BSS-3, recompute concurrency windows for other agents, take window with max concurrency, take 1 agent (first, random, whatever) and put it into BSS-1... etc - BSS-1, 2, 3 in round-robin manner.
// 4. for each BSS again compute concurrency windows, but now put them into same STA (assuming max perSta agent limit and rule about agent distribution).
DistributionResult
DistributeAgents(const ParsedResult& parsed,
                 int nAp = 3,
                 int nStationsPerAp = 30,
                 int maxAgentsPerStation = 3);

} // namespace ns3

#endif /* AGENT_DISTRIBUTION_H */
