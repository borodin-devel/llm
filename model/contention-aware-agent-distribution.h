#ifndef CONTENTION_AWARE_AGENT_DISTRIBUTION_H
#define CONTENTION_AWARE_AGENT_DISTRIBUTION_H

#include "agent-distribution.h"

namespace ns3
{

/**
 * Configuration for the contention-aware agent distribution algorithm.
 *
 * The algorithm has two independent stages:
 *
 * 1. BSS assignment
 *    Agents with overlapping UL activity are preferably placed into
 *    different BSSes.
 *
 * 2. STA assignment
 *    Depending on lowContentionPriority:
 *
 *    true:
 *      Prefer packing agents with overlapping UL activity onto the same STA.
 *      This reduces the number of distinct STAs contending for the medium.
 *
 *    false:
 *      Prefer using as many STAs as possible, even when spreading agents
 *      across STAs increases intra-BSS contention.
 *
 * UL activity is approximated using fixed-size time slots. An operation is
 * considered UL-active in the slot containing Operation::startOffsetMs,
 * matching the current uplink scheduling path.
 */
struct ContentionAwareDistributionConfig
{
    int nAp{3};                 ///< Number of independent BSS/AP groups.
    int nStationsPerAp{30};     ///< Number of stations available inside each BSS.
    int maxAgentsPerStation{3}; ///< Hard application-agent limit per STA.

    /**
     * STA placement policy.
     *
     * true:
     *   Minimize the number of distinct UL-active STAs per time slot.
     *
     * false:
     *   Maximize the number of STAs used by assigning agents to the currently
     *   least populated station.
     */
    bool lowContentionPriority{true};

    /**
     * Width of the time window used for approximate concurrency detection.
     *
     * Example:
     *
     *   slotMs = 50
     *
     *   startOffsetMs = 0.0..49.999...    -> slot 0
     *   startOffsetMs = 50.0..99.999...   -> slot 1
     *   startOffsetMs = 100.0..149.999... -> slot 2
     */
    int slotMs{50};
};

/**
 * Distribute agents using an UL-contention-aware heuristic.
 *
 * The original DistributeAgents() implementation is intentionally left
 * untouched so both algorithms can be run against the same trace.
 *
 * The returned DistributionResult uses exactly the same address layout as
 * the existing distributor:
 *
 *   AP 0: 10.1.0.1
 *   AP 1: 10.1.1.1
 *   AP 2: 10.1.2.1
 *
 * and:
 *
 *   STA 0: 10.1.<ap>.2:9000
 *   STA 1: 10.1.<ap>.3:9001
 *   ...
 *
 * @param parsedTrace Parsed agent trace.
 * @param config Distribution parameters.
 *
 * @return Agent -> BSS/STA mapping compatible with the existing scenario.
 *
 * @throws std::invalid_argument for invalid configuration or insufficient
 *         topology capacity.
 * @throws std::runtime_error if a complete assignment cannot be produced.
 */
DistributionResult DistributeAgentsContentionAware(
    const ParsedResult& parsedTrace,
    const ContentionAwareDistributionConfig& config = ContentionAwareDistributionConfig{});

} // namespace ns3

#endif // CONTENTION_AWARE_AGENT_DISTRIBUTION_H
