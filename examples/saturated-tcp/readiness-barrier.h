#ifndef SATURATED_TCP_READINESS_BARRIER_H
#define SATURATED_TCP_READINESS_BARRIER_H

#include "ns3/callback.h"
#include "ns3/event-id.h"
#include "ns3/ptr.h"

#include <cstdint>
#include <vector>

namespace ns3
{

class Application;
class SaturatedTcpSender;

/**
 * Coordinate saturated TCP readiness and one exact measurement second.
 *
 * Each sender receives a registration-specific readiness callback. Once all
 * callbacks have fired, the barrier selects the first whole-second traffic
 * boundary, arms statistics for the post-warm-up epoch, and starts all senders.
 */
class SaturatedReadinessBarrier
{
  public:
    /** Callback accepting an absolute simulation timestamp in nanoseconds. */
    using StatisticsCallback = Callback<void, int64_t>;

    /**
     * Construct an empty readiness barrier.
     *
     * @param startStatistics Callback that resets and starts statistics.
     * @param finalizeStatistics Callback that finalizes statistics.
     * @param trafficWarmupSeconds Saturated traffic time before statistics start.
     */
    SaturatedReadinessBarrier(StatisticsCallback startStatistics,
                              StatisticsCallback finalizeStatistics,
                              uint32_t trafficWarmupSeconds = 0);

    /** Cancel any pending readiness, epoch, or finalization events. */
    ~SaturatedReadinessBarrier();

    /**
     * Readiness barriers cannot be copied because scheduled events retain their owner.
     *
     * @param other Barrier that cannot be copied.
     */
    SaturatedReadinessBarrier(const SaturatedReadinessBarrier& other) = delete;

    /**
     * Readiness barriers cannot be copy-assigned because scheduled events retain their owner.
     *
     * @param other Barrier that cannot be assigned.
     * @return This barrier is never returned because assignment is deleted.
     */
    SaturatedReadinessBarrier& operator=(const SaturatedReadinessBarrier& other) = delete;

    /**
     * Register one readiness-gated sender.
     *
     * @param sender Sender application whose stop time follows the common epoch.
     * @param startTraffic Callback that opens this sender's payload gate.
     * @param stopTraffic Callback that closes this sender at the measurement endpoint.
     * @return One-shot callback through which this sender reports readiness.
     */
    Callback<void> RegisterSender(Ptr<SaturatedTcpSender> sender,
                                  Callback<void> startTraffic,
                                  Callback<void> stopTraffic);

    /**
     * Register a packet sink whose stop time and endpoint cleanup follow the common epoch.
     *
     * @param application Packet-sink application to coordinate.
     */
    void RegisterApplication(Ptr<Application> application);

    /** Lock registration, reject an empty barrier, and arm the post-retry safety timeout. */
    void FinalizeRegistration();

    /** @return Number of registered senders. */
    uint32_t GetRegisteredSenderCount() const;

    /** @return Number of senders that reported readiness. */
    uint32_t GetReadySenderCount() const;

    /** @return Number of coordinated sender and non-sender applications. */
    uint32_t GetRegisteredApplicationCount() const;

    /** @return Common measurement epoch in nanoseconds, or -1 before complete readiness. */
    int64_t GetExperimentStartNs() const;

    /** @return True after exact-endpoint statistics finalization. */
    bool IsMeasurementComplete() const;

  private:
    /** State and actions for one sender readiness registration. */
    struct SenderRegistration
    {
        Ptr<SaturatedTcpSender> sender; ///< Sender retained until its ready callback is cleared.
        Callback<void> startTraffic;    ///< Payload-gate callback invoked at the common epoch.
        Callback<void> stopTraffic;     ///< Sender cleanup callback invoked at the endpoint.
        bool ready{false};              ///< Whether this registration reported readiness.
    };

    /** State and endpoint action for one coordinated application. */
    struct ApplicationRegistration
    {
        Ptr<Application> application;   ///< Application retained through measurement finalization.
        Callback<void> stopApplication; ///< Executable endpoint cleanup, or null for a sender.
    };

    /**
     * Record one registration-specific readiness report.
     *
     * @param index Sender registration index.
     */
    void NotifyReady(uint32_t index);

    /** Start every sender at the selected traffic epoch. */
    void OpenBarrier();

    /** Open the measurement interval after the configured traffic warm-up. */
    void StartMeasurement();

    /** Stop senders, finalize statistics, and stop the simulator. */
    void FinalizeMeasurement();

    /** Fail because the fixed readiness safety deadline expired. */
    void ReadinessTimeout();

    std::vector<SenderRegistration> m_senders; ///< Ordered sender registrations.
    std::vector<ApplicationRegistration>
        m_applications;                      ///< All applications stopped at the endpoint.
    StatisticsCallback m_startStatistics;    ///< Future-epoch statistics arm action.
    StatisticsCallback m_finalizeStatistics; ///< Statistics exact-endpoint action.
    EventId m_safetyEvent;                   ///< Fixed readiness safety timeout.
    EventId m_openEvent;                     ///< Scheduled common-epoch event.
    EventId m_measurementStartEvent;         ///< Scheduled post-warm-up statistics event.
    EventId m_finalizeEvent;                 ///< Scheduled measurement-end event.
    uint32_t m_readySenderCount{0};          ///< Number of distinct ready registrations.
    uint32_t m_trafficWarmupSeconds{0};      ///< Traffic time excluded from statistics.
    int64_t m_trafficStartNs{-1};            ///< Common sender-start epoch in nanoseconds.
    int64_t m_experimentStartNs{-1};         ///< Measurement epoch in nanoseconds.
    bool m_registrationFinalized{false};     ///< Whether registration is locked.
    bool m_measurementComplete{false};       ///< Whether finalization completed.
};

} // namespace ns3

#endif // SATURATED_TCP_READINESS_BARRIER_H
