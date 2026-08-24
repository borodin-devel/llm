#ifndef TRAFFIC_COORDINATOR_H
#define TRAFFIC_COORDINATOR_H

#include "ns3/callback.h"
#include "ns3/ptr.h"

#include <cstdint>
#include <vector>

namespace ns3
{

class APGenerator;
class Application;
class StaLlmGenerator;

/**
 * Select the first integer-second boundary strictly after a time.
 *
 * @param nowMs Current simulation time in milliseconds.
 * @return Next integer-second boundary in milliseconds.
 */
int64_t GetNextIntegerSecondMs(int64_t nowMs);

/**
 * Coordinate the common traffic epoch after every TCP connection is ready.
 */
class TrafficCoordinator
{
  public:
    /**
     * Construct a coordinator for one simulation.
     *
     * @param traceDurationMs Complete input trace duration in milliseconds.
     * @param maxExperimentDurationMs Maximum traffic duration in milliseconds.
     */
    TrafficCoordinator(double traceDurationMs, double maxExperimentDurationMs);
    ~TrafficCoordinator();

    /** @return Callback that reports one generator ready. */
    Callback<void> GetReadyCallback();

    /** @param generator AP generator owned by an ns-3 node. */
    void AddGenerator(Ptr<APGenerator> generator);

    /** @param generator STA generator owned by an ns-3 node. */
    void AddGenerator(Ptr<StaLlmGenerator> generator);

    /** @param application Application whose stop time follows the common epoch. */
    void AddApplication(Ptr<Application> application);

    /** Lock registration and validate that at least one generator exists. */
    void FinalizeRegistration();

    /** @return Common trace epoch in microseconds, or -1 before readiness. */
    int64_t GetExperimentStartUs() const;

    /** @return Complete input trace duration in milliseconds. */
    double GetTraceDurationMs() const;

    /** @return Maximum traffic duration in milliseconds. */
    double GetMaxExperimentDurationMs() const;

    /** @return Number of generators required by the readiness barrier. */
    uint32_t GetExpectedGeneratorCount() const;

  private:
    /** Record one ready generator and open the barrier when complete. */
    void NotifyGeneratorReady();

    double m_traceDurationMs;         ///< Complete input trace duration in milliseconds.
    double m_maxExperimentDurationMs; ///< Maximum traffic duration in milliseconds.
    std::vector<Ptr<APGenerator>> m_apGenerators;      ///< Registered AP generators.
    std::vector<Ptr<StaLlmGenerator>> m_staGenerators; ///< Registered STA generators.
    std::vector<Ptr<Application>> m_applications; ///< Applications with coordinated stop times.
    uint32_t m_expectedGeneratorCount{0};         ///< Readiness barrier target.
    uint32_t m_readyGeneratorCount{0};            ///< Generators that reported readiness.
    int64_t m_experimentStartUs{-1};              ///< Common trace epoch in microseconds.
    bool m_registrationFinalized{false};          ///< Whether generator registration is locked.
};

} // namespace ns3

#endif // TRAFFIC_COORDINATOR_H
