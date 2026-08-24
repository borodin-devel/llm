#include "traffic-coordinator.h"

#include "scenario-log.h"

#include "ns3/ap-generator.h"
#include "ns3/application.h"
#include "ns3/simulator.h"
#include "ns3/sta-llm-generator.h"

namespace ns3
{

static LogComponent& g_log = llm_example::GetScenarioLog();

int64_t
GetNextIntegerSecondMs(int64_t nowMs)
{
    return ((nowMs / 1000) + 1) * 1000;
}

TrafficCoordinator::TrafficCoordinator(double traceDurationMs, double maxExperimentDurationMs)
    : m_traceDurationMs(traceDurationMs),
      m_maxExperimentDurationMs(maxExperimentDurationMs)
{
}

TrafficCoordinator::~TrafficCoordinator() = default;

Callback<void>
TrafficCoordinator::GetReadyCallback()
{
    return MakeCallback(&TrafficCoordinator::NotifyGeneratorReady, this);
}

void
TrafficCoordinator::AddGenerator(Ptr<APGenerator> generator)
{
    NS_ABORT_MSG_IF(m_registrationFinalized, "Cannot add an AP generator after registration");
    m_apGenerators.push_back(generator);
}

void
TrafficCoordinator::AddGenerator(Ptr<StaLlmGenerator> generator)
{
    NS_ABORT_MSG_IF(m_registrationFinalized, "Cannot add a STA generator after registration");
    m_staGenerators.push_back(generator);
}

void
TrafficCoordinator::AddApplication(Ptr<Application> application)
{
    NS_ABORT_MSG_IF(m_registrationFinalized, "Cannot add an application after registration");
    m_applications.push_back(application);
}

void
TrafficCoordinator::FinalizeRegistration()
{
    NS_ABORT_MSG_IF(m_registrationFinalized, "Traffic registration finalized more than once");
    m_registrationFinalized = true;
    m_expectedGeneratorCount =
        static_cast<uint32_t>(m_apGenerators.size() + m_staGenerators.size());
    NS_ABORT_MSG_IF(m_expectedGeneratorCount == 0, "No traffic generators were created");
}

int64_t
TrafficCoordinator::GetExperimentStartUs() const
{
    return m_experimentStartUs;
}

double
TrafficCoordinator::GetTraceDurationMs() const
{
    return m_traceDurationMs;
}

double
TrafficCoordinator::GetMaxExperimentDurationMs() const
{
    return m_maxExperimentDurationMs;
}

uint32_t
TrafficCoordinator::GetExpectedGeneratorCount() const
{
    return m_expectedGeneratorCount;
}

void
TrafficCoordinator::NotifyGeneratorReady()
{
    NS_ABORT_MSG_IF(!m_registrationFinalized, "Traffic readiness barrier was not configured");
    NS_ABORT_MSG_IF(m_readyGeneratorCount >= m_expectedGeneratorCount,
                    "Traffic generator reported readiness more than once");

    ++m_readyGeneratorCount;
    NS_LOG_INFO("[Traffic barrier] ready=" << m_readyGeneratorCount << "/"
                                           << m_expectedGeneratorCount);
    if (m_readyGeneratorCount != m_expectedGeneratorCount)
    {
        return;
    }

    const int64_t nowMs = Simulator::Now().GetMilliSeconds();
    const int64_t experimentStartMs = GetNextIntegerSecondMs(nowMs);
    m_experimentStartUs = experimentStartMs * 1000;
    const double applicationStopMs =
        static_cast<double>(experimentStartMs) + m_maxExperimentDurationMs;

    NS_LOG_INFO("[Traffic barrier] all TCP connections ready at "
                << nowMs << " ms; common traffic epoch=" << experimentStartMs << " ms; traceEnd="
                << m_traceDurationMs << " ms; maxExperimentTime=" << m_maxExperimentDurationMs
                << " ms; applicationStop=" << applicationStopMs << " ms");

    for (const auto& generator : m_staGenerators)
    {
        generator->StartTraffic(static_cast<uint64_t>(experimentStartMs));
    }
    for (const auto& generator : m_apGenerators)
    {
        generator->StartTraffic(static_cast<uint64_t>(experimentStartMs));
    }

    const Time applicationStopTime = Time::FromDouble(applicationStopMs, Time::MS);
    for (const auto& application : m_applications)
    {
        application->SetStopTime(applicationStopTime);
    }

    const Time simulatorStopDelay = applicationStopTime + MilliSeconds(1) - Simulator::Now();
    Simulator::Stop(simulatorStopDelay);
}

} // namespace ns3
