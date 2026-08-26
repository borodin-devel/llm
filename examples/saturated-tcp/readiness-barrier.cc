#include "readiness-barrier.h"

#include "ns3/application.h"
#include "ns3/log.h"
#include "ns3/nstime.h"
#include "ns3/simulator.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("SaturatedReadinessBarrier");

namespace
{

constexpr int64_t MEASUREMENT_DURATION_NS = 1'000'000'000;
constexpr int64_t READINESS_TIMEOUT_SECONDS = 30;

} // namespace

SaturatedReadinessBarrier::SaturatedReadinessBarrier(StatisticsCallback startStatistics,
                                                     StatisticsCallback finalizeStatistics)
    : m_startStatistics(std::move(startStatistics)),
      m_finalizeStatistics(std::move(finalizeStatistics))
{
    NS_ABORT_MSG_IF(m_startStatistics.IsNull(),
                    "saturated TCP statistics start callback is not configured");
    NS_ABORT_MSG_IF(m_finalizeStatistics.IsNull(),
                    "saturated TCP statistics finalize callback is not configured");
}

SaturatedReadinessBarrier::~SaturatedReadinessBarrier()
{
    m_safetyEvent.Cancel();
    m_openEvent.Cancel();
    m_finalizeEvent.Cancel();
}

Callback<void>
SaturatedReadinessBarrier::RegisterSender(Ptr<Application> application,
                                          Callback<void> startTraffic,
                                          Callback<void> stopTraffic)
{
    NS_ABORT_MSG_IF(m_registrationFinalized,
                    "cannot register saturated TCP sender after barrier finalization");
    NS_ABORT_MSG_IF(!application, "cannot register a null saturated TCP sender application");
    NS_ABORT_MSG_IF(startTraffic.IsNull(),
                    "cannot register saturated TCP sender without a start callback");
    NS_ABORT_MSG_IF(stopTraffic.IsNull(),
                    "cannot register saturated TCP sender without a stop callback");
    const auto duplicate = std::find(m_applications.begin(), m_applications.end(), application);
    NS_ABORT_MSG_IF(duplicate != m_applications.end(),
                    "saturated TCP application was registered more than once");

    const uint32_t index = static_cast<uint32_t>(m_senders.size());
    m_senders.push_back({application, std::move(startTraffic), std::move(stopTraffic), false});
    m_applications.push_back(application);
    return MakeCallback(&SaturatedReadinessBarrier::NotifyReady, this).Bind(index);
}

void
SaturatedReadinessBarrier::RegisterApplication(Ptr<Application> application)
{
    NS_ABORT_MSG_IF(m_registrationFinalized,
                    "cannot register saturated TCP application after barrier finalization");
    NS_ABORT_MSG_IF(!application, "cannot register a null saturated TCP application");
    const auto duplicate = std::find(m_applications.begin(), m_applications.end(), application);
    NS_ABORT_MSG_IF(duplicate != m_applications.end(),
                    "saturated TCP application was registered more than once");
    m_applications.push_back(application);
}

void
SaturatedReadinessBarrier::FinalizeRegistration()
{
    NS_ABORT_MSG_IF(m_registrationFinalized,
                    "saturated TCP readiness registration finalized more than once");
    NS_ABORT_MSG_IF(m_senders.empty(), "no saturated TCP senders were registered");
    m_registrationFinalized = true;
    m_safetyEvent = Simulator::Schedule(Seconds(READINESS_TIMEOUT_SECONDS),
                                        &SaturatedReadinessBarrier::ReadinessTimeout,
                                        this);
}

uint32_t
SaturatedReadinessBarrier::GetRegisteredSenderCount() const
{
    return static_cast<uint32_t>(m_senders.size());
}

uint32_t
SaturatedReadinessBarrier::GetReadySenderCount() const
{
    return m_readySenderCount;
}

uint32_t
SaturatedReadinessBarrier::GetRegisteredApplicationCount() const
{
    return static_cast<uint32_t>(m_applications.size());
}

int64_t
SaturatedReadinessBarrier::GetExperimentStartNs() const
{
    return m_experimentStartNs;
}

bool
SaturatedReadinessBarrier::IsMeasurementComplete() const
{
    return m_measurementComplete;
}

void
SaturatedReadinessBarrier::NotifyReady(uint32_t index)
{
    NS_ABORT_MSG_IF(!m_registrationFinalized,
                    "saturated TCP sender reported readiness before barrier finalization");
    NS_ABORT_MSG_IF(index >= m_senders.size(), "invalid saturated TCP sender readiness index");
    auto& sender = m_senders[index];
    NS_ABORT_MSG_IF(sender.ready, "saturated TCP sender reported readiness more than once");
    sender.ready = true;
    ++m_readySenderCount;
    if (m_readySenderCount != m_senders.size())
    {
        return;
    }

    m_safetyEvent.Cancel();
    const int64_t nowNs = Simulator::Now().GetNanoSeconds();
    NS_ABORT_MSG_IF(nowNs > std::numeric_limits<int64_t>::max() - MEASUREMENT_DURATION_NS,
                    "saturated TCP readiness epoch exceeds nanosecond range");
    m_experimentStartNs = ((nowNs / MEASUREMENT_DURATION_NS) + 1) * MEASUREMENT_DURATION_NS;
    const int64_t experimentEndNs = m_experimentStartNs + MEASUREMENT_DURATION_NS;
    for (const auto& application : m_applications)
    {
        application->SetStopTime(NanoSeconds(experimentEndNs));
    }
    m_openEvent = Simulator::Schedule(NanoSeconds(m_experimentStartNs - nowNs),
                                      &SaturatedReadinessBarrier::OpenBarrier,
                                      this);
}

void
SaturatedReadinessBarrier::OpenBarrier()
{
    NS_ABORT_MSG_IF(Simulator::Now().GetNanoSeconds() != m_experimentStartNs,
                    "saturated TCP readiness barrier opened outside its selected epoch");
    m_startStatistics(m_experimentStartNs);
    for (const auto& sender : m_senders)
    {
        sender.startTraffic();
    }
    m_finalizeEvent =
        Simulator::Schedule(Seconds(1), &SaturatedReadinessBarrier::FinalizeMeasurement, this);
}

void
SaturatedReadinessBarrier::FinalizeMeasurement()
{
    const int64_t experimentEndNs = m_experimentStartNs + MEASUREMENT_DURATION_NS;
    NS_ABORT_MSG_IF(Simulator::Now().GetNanoSeconds() != experimentEndNs,
                    "saturated TCP measurement did not end after exactly one second");
    for (const auto& sender : m_senders)
    {
        sender.stopTraffic();
    }
    m_finalizeStatistics(experimentEndNs);
    m_measurementComplete = true;
    Simulator::Stop();
}

void
SaturatedReadinessBarrier::ReadinessTimeout()
{
    NS_FATAL_ERROR("saturated TCP readiness timeout after 30 s: "
                   << m_readySenderCount << "/" << m_senders.size() << " senders ready");
}

} // namespace ns3
