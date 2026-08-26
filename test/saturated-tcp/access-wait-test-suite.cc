#include "../../examples/saturated-tcp/access-tracking-sta-wifi-mac.h"
#include "../../examples/saturated-tcp/access-wait-tracker.h"
#include "../llm-test-suite.h"

#include "ns3/boolean.h"
#include "ns3/channel-access-manager.h"
#include "ns3/default-power-save-manager.h"
#include "ns3/enum.h"
#include "ns3/pointer.h"
#include "ns3/qos-txop.h"
#include "ns3/simulator.h"
#include "ns3/txop.h"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <utility>
#include <vector>

using namespace ns3;

namespace
{

/** Power-save manager recording requests forwarded by the station MAC. */
class RecordingPowerSaveManager : public DefaultPowerSaveManager
{
  public:
    /**
     * Get the object TypeId.
     *
     * @return The object TypeId.
     */
    static TypeId GetTypeId();

    uint32_t requestCount{0}; ///< Number of forwarded access requests.
    Ptr<Txop> lastTxop;       ///< Most recently forwarded channel access function.
    uint8_t lastLinkId{0};    ///< Most recently forwarded link identifier.

  private:
    void DoNotifyRequestAccess(Ptr<Txop> txop, linkId_t linkId) override;
};

NS_OBJECT_ENSURE_REGISTERED(RecordingPowerSaveManager);

TypeId
RecordingPowerSaveManager::GetTypeId()
{
    static TypeId tid = TypeId("ns3::RecordingPowerSaveManager")
                            .SetParent<DefaultPowerSaveManager>()
                            .AddConstructor<RecordingPowerSaveManager>()
                            .HideFromDocumentation();
    return tid;
}

void
RecordingPowerSaveManager::DoNotifyRequestAccess(Ptr<Txop> txop, linkId_t linkId)
{
    ++requestCount;
    lastTxop = txop;
    lastLinkId = linkId;
}

/** Station MAC exposing the protected access notification to this test. */
class AccessTrackingStaWifiMacProbe : public AccessTrackingStaWifiMac
{
  public:
    /**
     * Get the object TypeId.
     *
     * @return The object TypeId.
     */
    static TypeId GetTypeId();

    /**
     * Invoke the channel-access notification.
     *
     * @param txop Channel access function requesting access.
     * @param linkId Link identifier.
     */
    void InvokeNotifyRequestAccess(Ptr<Txop> txop, uint8_t linkId);
};

NS_OBJECT_ENSURE_REGISTERED(AccessTrackingStaWifiMacProbe);

TypeId
AccessTrackingStaWifiMacProbe::GetTypeId()
{
    static TypeId tid = TypeId("ns3::AccessTrackingStaWifiMacProbe")
                            .SetParent<AccessTrackingStaWifiMac>()
                            .AddConstructor<AccessTrackingStaWifiMacProbe>()
                            .HideFromDocumentation();
    return tid;
}

void
AccessTrackingStaWifiMacProbe::InvokeNotifyRequestAccess(Ptr<Txop> txop, uint8_t linkId)
{
    NotifyRequestAccess(txop, linkId);
}

/** QoS channel access function with test-controlled active TXOP starts. */
class ControlledStartQosTxop : public QosTxop
{
  public:
    /**
     * Get the object TypeId.
     *
     * @return The object TypeId.
     */
    static TypeId GetTypeId();

    /**
     * Set the reported TXOP start on one link.
     *
     * @param linkId Link identifier.
     * @param startTime Active TXOP start, or no value when inactive.
     */
    void SetReportedTxopStartTime(uint8_t linkId, std::optional<Time> startTime);

    std::optional<Time> GetTxopStartTime(uint8_t linkId) const override;

  private:
    std::map<uint8_t, Time> m_startTimes; ///< Test-controlled active starts by link.
};

NS_OBJECT_ENSURE_REGISTERED(ControlledStartQosTxop);

TypeId
ControlledStartQosTxop::GetTypeId()
{
    static TypeId tid = TypeId("ns3::ControlledStartQosTxop")
                            .SetParent<QosTxop>()
                            .AddConstructor<ControlledStartQosTxop>()
                            .HideFromDocumentation();
    return tid;
}

void
ControlledStartQosTxop::SetReportedTxopStartTime(uint8_t linkId, std::optional<Time> startTime)
{
    if (startTime)
    {
        m_startTimes.insert_or_assign(linkId, *startTime);
    }
    else
    {
        m_startTimes.erase(linkId);
    }
}

std::optional<Time>
ControlledStartQosTxop::GetTxopStartTime(uint8_t linkId) const
{
    const auto start = m_startTimes.find(linkId);
    if (start == m_startTimes.end())
    {
        return std::nullopt;
    }
    return start->second;
}

/** Verify trace arguments, event cardinality, and base STA forwarding. */
class AccessTrackingStaWifiMacTestCase : public TestCase
{
  public:
    /** Construct the station MAC trace test. */
    AccessTrackingStaWifiMacTestCase();

  private:
    /**
     * Record one access-request trace event.
     *
     * @param ac Access category.
     * @param linkId Link identifier.
     */
    void RecordAccessRequest(uint8_t ac, uint8_t linkId);
    void DoRun() override;

    std::vector<std::pair<uint8_t, uint8_t>> m_requests; ///< Observed trace arguments.
};

AccessTrackingStaWifiMacTestCase::AccessTrackingStaWifiMacTestCase()
    : TestCase("saturated TCP station access-request trace")
{
}

void
AccessTrackingStaWifiMacTestCase::RecordAccessRequest(uint8_t ac, uint8_t linkId)
{
    m_requests.emplace_back(ac, linkId);
}

void
AccessTrackingStaWifiMacTestCase::DoRun()
{
    const auto makeQosTxop = [](AcIndex ac) {
        return CreateObjectWithAttributes<QosTxop>("AcIndex", EnumValue(ac));
    };
    const auto be = makeQosTxop(AC_BE);
    const auto bk = makeQosTxop(AC_BK);
    const auto vi = makeQosTxop(AC_VI);
    const auto vo = makeQosTxop(AC_VO);
    auto mac = CreateObjectWithAttributes<AccessTrackingStaWifiMacProbe>("QosSupported",
                                                                         BooleanValue(true),
                                                                         "BE_Txop",
                                                                         PointerValue(be),
                                                                         "BK_Txop",
                                                                         PointerValue(bk),
                                                                         "VI_Txop",
                                                                         PointerValue(vi),
                                                                         "VO_Txop",
                                                                         PointerValue(vo));
    auto powerSaveManager = CreateObject<RecordingPowerSaveManager>();
    mac->SetPowerSaveManager(powerSaveManager);

    NS_TEST_ASSERT_MSG_EQ(AccessTrackingStaWifiMac::GetTypeId().GetParent(),
                          StaWifiMac::GetTypeId(),
                          "Access-tracking MAC has the wrong TypeId parent");
    NS_TEST_ASSERT_MSG_EQ(TypeId::LookupByName("ns3::AccessTrackingStaWifiMac"),
                          AccessTrackingStaWifiMac::GetTypeId(),
                          "Access-tracking MAC TypeId is not registered by name");
    NS_TEST_ASSERT_MSG_EQ(
        mac->TraceConnectWithoutContext(
            "AccessRequested",
            MakeCallback(&AccessTrackingStaWifiMacTestCase::RecordAccessRequest, this)),
        true,
        "AccessRequested trace could not be connected");
    NS_TEST_ASSERT_MSG_EQ(m_requests.size(),
                          0,
                          "Attaching EDCA objects synthesized an access request");

    mac->InvokeNotifyRequestAccess(be, 3);
    mac->InvokeNotifyRequestAccess(be, 3);

    NS_TEST_ASSERT_MSG_EQ(m_requests.size(), 2, "Wrong number of access-request trace events");
    NS_TEST_ASSERT_MSG_EQ(+m_requests[0].first, +AC_BE, "Wrong access category in first event");
    NS_TEST_ASSERT_MSG_EQ(+m_requests[0].second, 3, "Wrong link in first event");
    NS_TEST_ASSERT_MSG_EQ(+m_requests[1].first, +AC_BE, "Wrong access category in second event");
    NS_TEST_ASSERT_MSG_EQ(+m_requests[1].second, 3, "Wrong link in second event");
    NS_TEST_ASSERT_MSG_EQ(powerSaveManager->requestCount,
                          2,
                          "Base station behavior did not receive every access request");
    NS_TEST_ASSERT_MSG_EQ(powerSaveManager->lastTxop,
                          be,
                          "Base station behavior received the wrong channel access function");
    NS_TEST_ASSERT_MSG_EQ(+powerSaveManager->lastLinkId,
                          3,
                          "Base station behavior received the wrong link");

    auto dcf = CreateObjectWithAttributes<Txop>("AcIndex", EnumValue(AC_BE_NQOS));
    mac->InvokeNotifyRequestAccess(dcf, 7);
    NS_TEST_ASSERT_MSG_EQ(m_requests.size(), 3, "Non-QoS request did not emit exactly one event");
    NS_TEST_ASSERT_MSG_EQ(+m_requests.back().first,
                          +AC_BE_NQOS,
                          "Non-QoS request has the wrong access category");
    NS_TEST_ASSERT_MSG_EQ(+m_requests.back().second, 7, "Non-QoS request has the wrong link");
    NS_TEST_ASSERT_MSG_EQ(powerSaveManager->requestCount,
                          3,
                          "Non-QoS request was not forwarded to base station behavior");
    NS_TEST_ASSERT_MSG_EQ(powerSaveManager->lastTxop,
                          dcf,
                          "Base station behavior received the wrong non-QoS function");
    mac->Dispose();
}

/** Verify first-request retention, historical grants, and cross-AC interval union. */
class AccessWaitUnionTestCase : public TestCase
{
  public:
    /** Construct the interval-union test. */
    AccessWaitUnionTestCase();

  private:
    void DoRun() override;
};

AccessWaitUnionTestCase::AccessWaitUnionTestCase()
    : TestCase("saturated TCP EDCA access-wait union")
{
}

void
AccessWaitUnionTestCase::DoRun()
{
    AccessWaitTracker tracker(0, 1000);
    tracker.NotifyRequest(AC_BE, 0, 100);
    tracker.NotifyGrant(AC_BE, 0, 300);
    tracker.NotifyGrant(AC_BE, 0, 350);

    tracker.NotifyRequest(AC_BE, 0, 400);
    tracker.NotifyRequest(AC_BE, 0, 425);
    tracker.NotifyRequest(AC_VI, 0, 450);
    tracker.NotifyGrant(AC_BE, 0, 500);
    tracker.NotifyGrant(AC_VI, 0, 700);

    tracker.NotifyGrant(AC_VO, 1, 750);
    tracker.NotifyRequest(AC_BK, 1, 800);
    tracker.Finalize();
    tracker.Finalize();

    const std::array<AccessWaitIntervalNs, 3> expected{{{100, 300}, {400, 700}, {800, 1000}}};
    const auto& actual = tracker.GetUnionIntervals();
    NS_TEST_ASSERT_MSG_EQ(actual.size(), expected.size(), "Wrong number of union intervals");
    for (std::size_t i = 0; i < expected.size(); ++i)
    {
        NS_TEST_ASSERT_MSG_EQ(actual[i].startNs, expected[i].startNs, "Wrong interval start");
        NS_TEST_ASSERT_MSG_EQ(actual[i].endNs, expected[i].endNs, "Wrong interval end");
    }
}

/** Verify clipping and ignoring TXOP continuations without matching requests. */
class AccessWaitClippingTestCase : public TestCase
{
  public:
    /** Construct the clipping test. */
    AccessWaitClippingTestCase();

  private:
    void DoRun() override;
};

AccessWaitClippingTestCase::AccessWaitClippingTestCase()
    : TestCase("saturated TCP EDCA access-wait measurement clipping")
{
}

void
AccessWaitClippingTestCase::DoRun()
{
    AccessWaitTracker tracker(1000, 2000);
    tracker.NotifyRequest(AC_BK, 0, 100);
    tracker.NotifyGrant(AC_BK, 0, 900);
    tracker.NotifyRequest(AC_BE, 0, 900);
    tracker.NotifyGrant(AC_BE, 0, 1100);

    tracker.NotifyRequest(AC_VI, 0, 1300);
    tracker.NotifyGrant(AC_VI, 0, 1250);
    tracker.NotifyGrant(AC_VI, 0, 1400);
    tracker.NotifyGrant(AC_VO, 0, 1500);

    tracker.NotifyRequest(AC_BE, 1, 1900);
    tracker.NotifyGrant(AC_BE, 1, 2100);
    tracker.NotifyRequest(AC_BK, 1, 2200);
    tracker.Finalize();

    const std::array<AccessWaitIntervalNs, 3> expected{{{1000, 1100}, {1300, 1400}, {1900, 2000}}};
    const auto& actual = tracker.GetUnionIntervals();
    NS_TEST_ASSERT_MSG_EQ(actual.size(), expected.size(), "Wrong number of clipped intervals");
    for (std::size_t i = 0; i < expected.size(); ++i)
    {
        NS_TEST_ASSERT_MSG_EQ(actual[i].startNs, expected[i].startNs, "Wrong clipped start");
        NS_TEST_ASSERT_MSG_EQ(actual[i].endNs, expected[i].endNs, "Wrong clipped end");
        NS_TEST_ASSERT_MSG_EQ(actual[i].startNs >= 1000,
                              true,
                              "Interval starts before the measurement epoch");
        NS_TEST_ASSERT_MSG_EQ(actual[i].endNs <= 2000,
                              true,
                              "Interval ends after the measurement epoch");
    }
}

/** Verify an active grant is reconciled before a release-delayed TXOP trace. */
class AccessWaitDelayedReleaseTestCase : public TestCase
{
  public:
    /** Construct the delayed-release regression test. */
    AccessWaitDelayedReleaseTestCase();

  private:
    void DoRun() override;
};

AccessWaitDelayedReleaseTestCase::AccessWaitDelayedReleaseTestCase()
    : TestCase("saturated TCP active TXOP before delayed release trace")
{
}

void
AccessWaitDelayedReleaseTestCase::DoRun()
{
    const auto makeQosTxop = [](AcIndex ac) {
        return CreateObjectWithAttributes<ControlledStartQosTxop>("AcIndex", EnumValue(ac));
    };
    const auto be = makeQosTxop(AC_BE);
    auto mac =
        CreateObjectWithAttributes<AccessTrackingStaWifiMacProbe>("QosSupported",
                                                                  BooleanValue(true),
                                                                  "BE_Txop",
                                                                  PointerValue(be),
                                                                  "BK_Txop",
                                                                  PointerValue(makeQosTxop(AC_BK)),
                                                                  "VI_Txop",
                                                                  PointerValue(makeQosTxop(AC_VI)),
                                                                  "VO_Txop",
                                                                  PointerValue(makeQosTxop(AC_VO)));
    mac->SetChannelAccessManagers({CreateObject<ChannelAccessManager>()});

    AccessWaitTracker tracker(0, 1000);
    Simulator::Schedule(NanoSeconds(800), [&tracker] {
        tracker.NotifyRequest(AC_BE, 0, Simulator::Now().GetNanoSeconds());
    });
    Simulator::Schedule(NanoSeconds(900),
                        [be] { be->SetReportedTxopStartTime(0, Simulator::Now()); });
    Simulator::Schedule(NanoSeconds(1000),
                        [&tracker, mac] { tracker.Finalize(mac->GetActiveTxopStartTimes()); });
    Simulator::Schedule(NanoSeconds(1100), [&tracker, be] {
        be->SetReportedTxopStartTime(0, std::nullopt);
        tracker.NotifyGrant(AC_BE, 0, NanoSeconds(900).GetNanoSeconds());
    });
    Simulator::Run();

    const auto& intervals = tracker.GetUnionIntervals();
    NS_TEST_ASSERT_MSG_EQ(intervals.size(), 1, "Delayed release produced the wrong interval count");
    NS_TEST_ASSERT_MSG_EQ(intervals[0].startNs, 800, "Delayed release changed the request time");
    NS_TEST_ASSERT_MSG_EQ(intervals[0].endNs,
                          900,
                          "Delayed release extended waiting through measurement finalization");
    mac->Dispose();
    Simulator::Destroy();
}

/** Verify every concurrently active access category is reconciled. */
class AccessWaitMultipleActiveAcTestCase : public TestCase
{
  public:
    /** Construct the multiple-active-AC regression test. */
    AccessWaitMultipleActiveAcTestCase();

  private:
    void DoRun() override;
};

AccessWaitMultipleActiveAcTestCase::AccessWaitMultipleActiveAcTestCase()
    : TestCase("saturated TCP multiple active TXOP reconciliation")
{
}

void
AccessWaitMultipleActiveAcTestCase::DoRun()
{
    const auto makeQosTxop = [](AcIndex ac) {
        return CreateObjectWithAttributes<ControlledStartQosTxop>("AcIndex", EnumValue(ac));
    };
    const auto be = makeQosTxop(AC_BE);
    const auto vi = makeQosTxop(AC_VI);
    auto mac =
        CreateObjectWithAttributes<AccessTrackingStaWifiMacProbe>("QosSupported",
                                                                  BooleanValue(true),
                                                                  "BE_Txop",
                                                                  PointerValue(be),
                                                                  "BK_Txop",
                                                                  PointerValue(makeQosTxop(AC_BK)),
                                                                  "VI_Txop",
                                                                  PointerValue(vi),
                                                                  "VO_Txop",
                                                                  PointerValue(makeQosTxop(AC_VO)));
    mac->SetChannelAccessManagers({CreateObject<ChannelAccessManager>()});

    AccessWaitTracker tracker(0, 1000);
    tracker.NotifyRequest(AC_BE, 0, 800);
    tracker.NotifyRequest(AC_VI, 0, 825);
    be->SetReportedTxopStartTime(0, NanoSeconds(900));
    vi->SetReportedTxopStartTime(0, NanoSeconds(950));
    tracker.Finalize(mac->GetActiveTxopStartTimes());

    const auto& intervals = tracker.GetUnionIntervals();
    NS_TEST_ASSERT_MSG_EQ(intervals.size(), 1, "Active access categories were not unioned");
    NS_TEST_ASSERT_MSG_EQ(intervals[0].startNs, 800, "Wrong multiple-AC union start");
    NS_TEST_ASSERT_MSG_EQ(intervals[0].endNs, 950, "Not every active access category was closed");
    mac->Dispose();
}

} // namespace

std::vector<TestCase*>
CreateSaturatedTcpAccessWaitTestCases()
{
    return {
        new AccessTrackingStaWifiMacTestCase(),
        new AccessWaitUnionTestCase(),
        new AccessWaitClippingTestCase(),
        new AccessWaitDelayedReleaseTestCase(),
        new AccessWaitMultipleActiveAcTestCase(),
    };
}
