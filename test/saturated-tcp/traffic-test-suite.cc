#include "../../examples/saturated-tcp/readiness-barrier.h"
#include "../../examples/saturated-tcp/saturated-tcp-sender.h"
#include "../../examples/saturated-tcp/traffic.h"
#include "../llm-test-suite.h"

#include "ns3/address.h"
#include "ns3/application.h"
#include "ns3/config.h"
#include "ns3/internet-stack-helper.h"
#include "ns3/ipv4-address-helper.h"
#include "ns3/packet-sink-helper.h"
#include "ns3/packet-sink.h"
#include "ns3/point-to-point-helper.h"
#include "ns3/simulator.h"
#include "ns3/socket.h"
#include "ns3/string.h"
#include "ns3/tcp-socket-base.h"
#include "ns3/tcp-socket-factory.h"
#include "ns3/tcp-socket.h"
#include "ns3/uinteger.h"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#ifdef __unix__
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace ns3;

namespace
{

/** Result from one subprocess-isolated fatal-path operation. */
struct FatalPathResult
{
    bool launched{false};   ///< Whether the child process was created.
    bool failed{false};     ///< Whether the child terminated unsuccessfully.
    std::string diagnostic; ///< Standard-error text emitted by the child.
};

/**
 * Run an operation in a child process and capture its fatal diagnostic.
 *
 * @param operation Operation expected to terminate unsuccessfully.
 * @return Child launch, status, and standard-error result.
 */
FatalPathResult
RunFatalPath(const std::function<void()>& operation)
{
#ifdef __unix__
    int descriptors[2];
    if (pipe(descriptors) != 0)
    {
        return {};
    }

    const pid_t child = fork();
    if (child < 0)
    {
        close(descriptors[0]);
        close(descriptors[1]);
        return {};
    }
    if (child == 0)
    {
        close(descriptors[0]);
        if (dup2(descriptors[1], STDERR_FILENO) < 0)
        {
            std::_Exit(EXIT_FAILURE);
        }
        close(descriptors[1]);
        operation();
        Simulator::Destroy();
        std::_Exit(EXIT_SUCCESS);
    }

    close(descriptors[1]);
    std::string diagnostic;
    std::array<char, 512> buffer{};
    while (true)
    {
        const ssize_t count = read(descriptors[0], buffer.data(), buffer.size());
        if (count > 0)
        {
            diagnostic.append(buffer.data(), static_cast<std::size_t>(count));
            continue;
        }
        if (count < 0 && errno == EINTR)
        {
            continue;
        }
        break;
    }
    close(descriptors[0]);

    int status = 0;
    if (waitpid(child, &status, 0) != child)
    {
        return {};
    }
    const bool failed =
        WIFSIGNALED(status) || (WIFEXITED(status) && WEXITSTATUS(status) != EXIT_SUCCESS);
    return {true, failed, std::move(diagnostic)};
#else
    static_cast<void>(operation);
    return {};
#endif
}

/** Two-node TCP fixture used to exercise the real socket lifecycle. */
struct SenderFixture
{
    NodeContainer nodes;               ///< Source and destination nodes.
    Ipv4InterfaceContainer interfaces; ///< Point-to-point IPv4 interfaces.
    Ptr<PacketSink> sink;              ///< Optional destination sink.
    Ptr<SaturatedTcpSender> sender;    ///< Saturated source application.
};

/**
 * Build a routed two-node TCP sender fixture.
 *
 * @param installSink Whether to listen at the remote endpoint.
 * @param readyCallback Sender readiness callback.
 * @return Configured sender fixture whose applications start at time zero.
 */
SenderFixture
BuildSenderFixture(bool installSink, const Callback<void>& readyCallback)
{
    SenderFixture fixture;
    fixture.nodes.Create(2);

    PointToPointHelper pointToPoint;
    pointToPoint.SetDeviceAttribute("DataRate", StringValue("5Mbps"));
    pointToPoint.SetChannelAttribute("Delay", StringValue("1ms"));
    const NetDeviceContainer devices = pointToPoint.Install(fixture.nodes);

    InternetStackHelper internet;
    internet.Install(fixture.nodes);
    Ipv4AddressHelper addresses;
    addresses.SetBase("10.71.0.0", "255.255.255.0");
    fixture.interfaces = addresses.Assign(devices);

    constexpr uint16_t destinationPort = 21000;
    if (installSink)
    {
        PacketSinkHelper sinkHelper(
            "ns3::TcpSocketFactory",
            InetSocketAddress(fixture.interfaces.GetAddress(1), destinationPort));
        const ApplicationContainer sinks = sinkHelper.Install(fixture.nodes.Get(1));
        fixture.sink = DynamicCast<PacketSink>(sinks.Get(0));
        fixture.sink->SetStartTime(Seconds(0));
        fixture.sink->SetStopTime(MilliSeconds(480));
    }

    fixture.sender = CreateObject<SaturatedTcpSender>();
    fixture.sender->SetAttribute(
        "Local",
        AddressValue(InetSocketAddress(fixture.interfaces.GetAddress(0), 11000)));
    fixture.sender->SetRemote(InetSocketAddress(fixture.interfaces.GetAddress(1), destinationPort));
    fixture.sender->SetAttribute("SendSize", UintegerValue(512));
    fixture.sender->SetReadyCallback(readyCallback);
    fixture.nodes.Get(0)->AddApplication(fixture.sender);
    fixture.sender->SetStartTime(Seconds(0));
    return fixture;
}

/**
 * Start the same sender twice from its connection-ready callback.
 *
 * @param sender Sender whose traffic gate is opened twice.
 */
void
StartSenderTwice(Ptr<SaturatedTcpSender> sender)
{
    sender->StartTraffic();
    sender->StartTraffic();
}

/** Ignore one sender readiness report. */
void
IgnoreReady()
{
}

/**
 * Increment one readiness counter.
 *
 * @param count Counter to increment.
 */
void
CountReady(uint32_t* count)
{
    ++*count;
}

/**
 * Record one source-application connection event.
 *
 * @param count Event counter to increment.
 * @param capturedSocket Destination for the event socket.
 * @param capturedLocal Destination for the local endpoint.
 * @param capturedRemote Destination for the remote endpoint.
 * @param socket Event socket.
 * @param local Local endpoint.
 * @param remote Remote endpoint.
 */
void
RecordConnectionEvent(uint32_t* count,
                      Ptr<Socket>* capturedSocket,
                      Address* capturedLocal,
                      Address* capturedRemote,
                      Ptr<Socket> socket,
                      const Address& local,
                      const Address& remote)
{
    ++*count;
    *capturedSocket = socket;
    *capturedLocal = local;
    *capturedRemote = remote;
}

/**
 * @ingroup tests
 *
 * Verify readiness gating, unlimited refill, and stop cleanup over real TCP.
 */
class SaturatedTcpSenderLifecycleTestCase : public TestCase
{
  public:
    /** Construct the real TCP lifecycle test. */
    SaturatedTcpSenderLifecycleTestCase();

  private:
    /** Record connection readiness and shrink the live TCP send buffer. */
    void NotifyReady();
    /** Record destination bytes while traffic remains gated. */
    void RecordPreTrafficBytes();
    /** Open the application traffic gate. */
    void StartTraffic();
    /** Probe the closed socket after the application stop event. */
    void ProbeClosedSocket();
    /** Record destination bytes after all in-flight data has drained. */
    void RecordSettledBytes();
    /** Record destination bytes immediately before simulation stop. */
    void RecordFinalBytes();
    void DoRun() override;

    SenderFixture m_fixture;       ///< Active real TCP fixture.
    uint32_t m_readyCount{0};      ///< Number of readiness reports.
    int64_t m_readyNs{-1};         ///< Readiness simulation timestamp.
    uint64_t m_bytesAtReady{0};    ///< Payload observed at readiness.
    uint64_t m_preTrafficBytes{0}; ///< Payload observed before gate opening.
    uint64_t m_settledBytes{0};    ///< Payload after the stopped flow drains.
    uint64_t m_finalBytes{0};      ///< Payload immediately before simulation stop.
    int m_closedSendResult{0};     ///< Send result after application stop.
};

SaturatedTcpSenderLifecycleTestCase::SaturatedTcpSenderLifecycleTestCase()
    : TestCase("gate and resume a saturated sender over real TCP")
{
}

void
SaturatedTcpSenderLifecycleTestCase::NotifyReady()
{
    ++m_readyCount;
    m_readyNs = Simulator::Now().GetNanoSeconds();
    m_bytesAtReady = m_fixture.sink->GetTotalRx();
    m_fixture.sender->GetSocket()->SetAttribute("SndBufSize", UintegerValue(2048));
}

void
SaturatedTcpSenderLifecycleTestCase::RecordPreTrafficBytes()
{
    m_preTrafficBytes = m_fixture.sink->GetTotalRx();
}

void
SaturatedTcpSenderLifecycleTestCase::StartTraffic()
{
    m_fixture.sender->StartTraffic();
}

void
SaturatedTcpSenderLifecycleTestCase::ProbeClosedSocket()
{
    m_closedSendResult = m_fixture.sender->GetSocket()->Send(Create<Packet>(1));
    m_settledBytes = m_fixture.sink->GetTotalRx();
}

void
SaturatedTcpSenderLifecycleTestCase::RecordSettledBytes()
{
    m_settledBytes = m_fixture.sink->GetTotalRx();
}

void
SaturatedTcpSenderLifecycleTestCase::RecordFinalBytes()
{
    m_finalBytes = m_fixture.sink->GetTotalRx();
}

void
SaturatedTcpSenderLifecycleTestCase::DoRun()
{
    Simulator::Destroy();
    m_fixture =
        BuildSenderFixture(true,
                           MakeCallback(&SaturatedTcpSenderLifecycleTestCase::NotifyReady, this));
    m_fixture.sender->SetStopTime(MilliSeconds(250));

    Simulator::Schedule(MilliSeconds(90),
                        &SaturatedTcpSenderLifecycleTestCase::RecordPreTrafficBytes,
                        this);
    Simulator::Schedule(MilliSeconds(100),
                        &SaturatedTcpSenderLifecycleTestCase::StartTraffic,
                        this);
    Simulator::Schedule(MilliSeconds(300),
                        &SaturatedTcpSenderLifecycleTestCase::ProbeClosedSocket,
                        this);
    Simulator::Schedule(MilliSeconds(400),
                        &SaturatedTcpSenderLifecycleTestCase::RecordSettledBytes,
                        this);
    Simulator::Schedule(MilliSeconds(470),
                        &SaturatedTcpSenderLifecycleTestCase::RecordFinalBytes,
                        this);
    Simulator::Stop(MilliSeconds(475));
    Simulator::Run();

    NS_TEST_ASSERT_MSG_EQ(m_readyCount, 1, "Sender did not report readiness exactly once");
    NS_TEST_ASSERT_MSG_GT_OR_EQ(m_readyNs, 0, "TCP connection never became ready");
    NS_TEST_ASSERT_MSG_LT(m_readyNs,
                          MilliSeconds(90).GetNanoSeconds(),
                          "TCP connection was not initiated at application start");
    NS_TEST_ASSERT_MSG_EQ(m_bytesAtReady, 0, "Sender emitted payload from its ready callback");
    NS_TEST_ASSERT_MSG_EQ(m_preTrafficBytes, 0, "Sender emitted payload before StartTraffic");
    NS_TEST_ASSERT_MSG_GT(m_finalBytes,
                          4 * 2048,
                          "Sender did not resume after filling the TCP send buffer");
    NS_TEST_ASSERT_MSG_EQ(m_closedSendResult, -1, "Sender socket remained writable after stop");
    NS_TEST_ASSERT_MSG_EQ(m_finalBytes,
                          m_settledBytes,
                          "Sender delivered payload after stop cleanup settled");
    Simulator::Destroy();
}

/**
 * @ingroup tests
 *
 * Verify that traffic cannot start before TCP connection readiness.
 */
class SaturatedTcpSenderBeforeReadyTestCase : public TestCase
{
  public:
    /** Construct the pre-readiness fatal-state test. */
    SaturatedTcpSenderBeforeReadyTestCase();

  private:
    void DoRun() override;
};

SaturatedTcpSenderBeforeReadyTestCase::SaturatedTcpSenderBeforeReadyTestCase()
    : TestCase("reject saturated traffic before connection readiness")
{
}

void
SaturatedTcpSenderBeforeReadyTestCase::DoRun()
{
#ifdef __unix__
    const auto result = RunFatalPath([] {
        auto sender = CreateObject<SaturatedTcpSender>();
        sender->StartTraffic();
    });
    NS_TEST_ASSERT_MSG_EQ(result.launched, true, "Could not create fatal-path child process");
    NS_TEST_ASSERT_MSG_EQ(result.failed, true, "StartTraffic accepted a sender before readiness");
    NS_TEST_ASSERT_MSG_NE(result.diagnostic.find("before connection readiness"),
                          std::string::npos,
                          "Pre-readiness failure did not explain the invalid state");
#endif
}

/**
 * @ingroup tests
 *
 * Verify that the application traffic gate can open only once.
 */
class SaturatedTcpSenderDuplicateStartTestCase : public TestCase
{
  public:
    /** Construct the duplicate-start fatal-state test. */
    SaturatedTcpSenderDuplicateStartTestCase();

  private:
    void DoRun() override;
};

SaturatedTcpSenderDuplicateStartTestCase::SaturatedTcpSenderDuplicateStartTestCase()
    : TestCase("reject duplicate saturated traffic start")
{
}

void
SaturatedTcpSenderDuplicateStartTestCase::DoRun()
{
#ifdef __unix__
    const auto result = RunFatalPath([] {
        auto fixture = BuildSenderFixture(true, Callback<void>());
        fixture.sender->SetReadyCallback(MakeCallback(&StartSenderTwice).Bind(fixture.sender));
        Simulator::Stop(Seconds(1));
        Simulator::Run();
    });
    NS_TEST_ASSERT_MSG_EQ(result.launched, true, "Could not create fatal-path child process");
    NS_TEST_ASSERT_MSG_EQ(result.failed, true, "StartTraffic accepted a duplicate call");
    NS_TEST_ASSERT_MSG_NE(result.diagnostic.find("started more than once"),
                          std::string::npos,
                          "Duplicate-start failure did not explain the invalid state");
#endif
}

/**
 * @ingroup tests
 *
 * Verify that a failed TCP cohort reconnects with a fresh configured socket.
 */
class SaturatedTcpSenderConnectionRetryTestCase : public TestCase
{
  public:
    /** Construct the fresh-socket retry test. */
    SaturatedTcpSenderConnectionRetryTestCase();

  private:
    void DoRun() override;
};

SaturatedTcpSenderConnectionRetryTestCase::SaturatedTcpSenderConnectionRetryTestCase()
    : TestCase("retry saturated TCP readiness with a fresh configured socket")
{
}

void
SaturatedTcpSenderConnectionRetryTestCase::DoRun()
{
#ifdef __unix__
    const auto result = RunFatalPath([] {
        Config::SetDefault("ns3::TcpSocket::ConnTimeout", TimeValue(MilliSeconds(10)));
        Config::SetDefault("ns3::TcpSocket::ConnCount", UintegerValue(1));
        uint32_t readyCount = 0;
        uint32_t failureCount = 0;
        uint32_t successCount = 0;
        Ptr<Socket> failedSocket;
        Ptr<Socket> successfulSocket;
        Address failedLocal;
        Address failedRemote;
        Address successfulLocal;
        Address successfulRemote;

        auto fixture = BuildSenderFixture(true, MakeCallback(&CountReady).Bind(&readyCount));
        fixture.sink->SetStartTime(MilliSeconds(100));
        fixture.sink->SetStopTime(Seconds(2));
        fixture.sender->SetAttribute("Tos", UintegerValue(0x28));
        fixture.sender->TraceConnectWithoutContext(
            "ConnectionFailed",
            MakeCallback(&RecordConnectionEvent)
                .Bind(&failureCount, &failedSocket, &failedLocal, &failedRemote));
        fixture.sender->TraceConnectWithoutContext(
            "ConnectionSucceeded",
            MakeCallback(&RecordConnectionEvent)
                .Bind(&successCount, &successfulSocket, &successfulLocal, &successfulRemote));
        Simulator::Stop(Seconds(2));
        Simulator::Run();

        const auto expectedLocal = InetSocketAddress(fixture.interfaces.GetAddress(0), 11000);
        const auto expectedRemote = InetSocketAddress(fixture.interfaces.GetAddress(1), 21000);
        const auto actualLocal = InetSocketAddress::ConvertFrom(successfulLocal);
        const auto actualRemote = InetSocketAddress::ConvertFrom(successfulRemote);
        if (failureCount != 1 || successCount != 1 || readyCount != 1 || !failedSocket ||
            !successfulSocket || failedSocket == successfulSocket ||
            !InetSocketAddress::IsMatchingType(successfulLocal) ||
            actualLocal.GetIpv4() != expectedLocal.GetIpv4() ||
            actualLocal.GetPort() != expectedLocal.GetPort() ||
            !InetSocketAddress::IsMatchingType(successfulRemote) ||
            actualRemote.GetIpv4() != expectedRemote.GetIpv4() ||
            actualRemote.GetPort() != expectedRemote.GetPort() ||
            successfulSocket->GetIpTos() != 0x28 || fixture.sink->GetTotalRx() != 0)
        {
            std::_Exit(EXIT_FAILURE);
        }
    });
    NS_TEST_ASSERT_MSG_EQ(result.launched, true, "Could not create reconnect child process");
    NS_TEST_ASSERT_MSG_EQ(result.failed,
                          false,
                          "Fresh-socket readiness retry failed: " << result.diagnostic);
#endif
}

/**
 * @ingroup tests
 *
 * Verify that sender stop cancels a pending fresh-socket retry.
 */
class SaturatedTcpSenderRetryCancellationTestCase : public TestCase
{
  public:
    /** Construct the pending-retry cancellation test. */
    SaturatedTcpSenderRetryCancellationTestCase();

  private:
    void DoRun() override;
};

SaturatedTcpSenderRetryCancellationTestCase::SaturatedTcpSenderRetryCancellationTestCase()
    : TestCase("cancel saturated TCP reconnect when the sender stops")
{
}

void
SaturatedTcpSenderRetryCancellationTestCase::DoRun()
{
#ifdef __unix__
    const auto result = RunFatalPath([] {
        Config::SetDefault("ns3::TcpSocket::ConnTimeout", TimeValue(MilliSeconds(10)));
        Config::SetDefault("ns3::TcpSocket::ConnCount", UintegerValue(1));
        uint32_t readyCount = 0;
        uint32_t failureCount = 0;
        uint32_t successCount = 0;
        Ptr<Socket> failedSocket;
        Ptr<Socket> successfulSocket;
        Address failedLocal;
        Address failedRemote;
        Address successfulLocal;
        Address successfulRemote;

        auto fixture = BuildSenderFixture(false, MakeCallback(&CountReady).Bind(&readyCount));
        fixture.sender->TraceConnectWithoutContext(
            "ConnectionFailed",
            MakeCallback(&RecordConnectionEvent)
                .Bind(&failureCount, &failedSocket, &failedLocal, &failedRemote));
        fixture.sender->TraceConnectWithoutContext(
            "ConnectionSucceeded",
            MakeCallback(&RecordConnectionEvent)
                .Bind(&successCount, &successfulSocket, &successfulLocal, &successfulRemote));
        fixture.sender->SetStopTime(MilliSeconds(100));
        Simulator::Stop(Seconds(2));
        Simulator::Run();
        if (failureCount != 1 || successCount != 0 || readyCount != 0)
        {
            std::_Exit(EXIT_FAILURE);
        }
    });
    NS_TEST_ASSERT_MSG_EQ(result.launched, true, "Could not create retry-cancellation child");
    NS_TEST_ASSERT_MSG_EQ(result.failed,
                          false,
                          "Stopped sender retained a stale reconnect: " << result.diagnostic);
#endif
}

/**
 * @ingroup tests
 *
 * Verify that the barrier closes real packet-sink endpoints before statistics finalization.
 */
class SaturatedTcpSinkEndpointCleanupTestCase : public TestCase
{
  public:
    /** Construct the real sink endpoint-cleanup test. */
    SaturatedTcpSinkEndpointCleanupTestCase();

  private:
    /**
     * Record statistics start.
     *
     * @param epochNs Common measurement epoch in nanoseconds.
     */
    void StartStatistics(int64_t epochNs);
    /**
     * Inspect sender and sink state from the statistics-finalize callback.
     *
     * @param endNs Exact measurement endpoint in nanoseconds.
     */
    void FinalizeStatistics(int64_t endNs);
    /** Stop the real sender while recording endpoint callback order. */
    void StopSenderAtEndpoint();
    /** Retain and trace the real listening and accepted sink sockets. */
    void CaptureSinkSockets();
    /**
     * Record a real sink TCP state transition.
     *
     * @param listening Whether the transition belongs to the listening socket.
     * @param oldState Previous TCP state.
     * @param newState New TCP state.
     */
    void NotifySinkState(bool listening,
                         TcpSocket::TcpStates_t oldState,
                         TcpSocket::TcpStates_t newState);
    /**
     * Record a post-end connection and offer payload to the supposedly closed sink.
     *
     * @param socket Probe client socket.
     */
    void ProbeConnectionSucceeded(Ptr<Socket> socket);
    /**
     * Record rejection of a post-end connection.
     *
     * @param socket Probe client socket.
     */
    void ProbeConnectionFailed(Ptr<Socket> socket);
    void DoRun() override;

    SenderFixture m_fixture;         ///< Real sender and packet-sink fixture.
    Ptr<Socket> m_listeningSocket;   ///< Sink listening socket retained before endpoint.
    Ptr<Socket> m_acceptedSocket;    ///< Sink accepted socket retained before endpoint.
    Ptr<Socket> m_probeSocket;       ///< Post-end client connection probe.
    int64_t m_statisticsStartNs{-1}; ///< Observed statistics start timestamp.
    int64_t m_statisticsEndNs{-1};   ///< Observed statistics finalize timestamp.
    int64_t m_senderStopNs{-1};      ///< Observed sender cleanup timestamp.
    uint64_t m_bytesAtEndpoint{0};   ///< Sink total when statistics finalize.
    uint64_t m_bytesAfterProbe{0};   ///< Sink total after post-end probe traffic.
    bool m_listeningClosed{false};   ///< Whether listener entered CLOSED at the endpoint.
    bool m_acceptedClosing{false};   ///< Whether accepted socket left ESTABLISHED at endpoint.
    bool m_probeConnected{false};    ///< Whether the post-end probe connected.
    bool m_probeFailed{false};       ///< Whether the post-end probe was rejected.
    std::vector<std::string> m_endpointOrder; ///< Sender, sink, and statistics endpoint order.
};

SaturatedTcpSinkEndpointCleanupTestCase::SaturatedTcpSinkEndpointCleanupTestCase()
    : TestCase("close saturated packet-sink endpoints before statistics finalization")
{
}

void
SaturatedTcpSinkEndpointCleanupTestCase::StartStatistics(int64_t epochNs)
{
    m_statisticsStartNs = Simulator::Now().GetNanoSeconds();
    NS_TEST_ASSERT_MSG_EQ(m_statisticsStartNs,
                          epochNs,
                          "Statistics start argument diverged from event time");
}

void
SaturatedTcpSinkEndpointCleanupTestCase::FinalizeStatistics(int64_t endNs)
{
    m_statisticsEndNs = Simulator::Now().GetNanoSeconds();
    m_bytesAtEndpoint = m_fixture.sink->GetTotalRx();
    m_endpointOrder.push_back("statistics-finalize");
    NS_TEST_ASSERT_MSG_EQ(m_statisticsEndNs,
                          endNs,
                          "Statistics finalize argument diverged from event time");
}

void
SaturatedTcpSinkEndpointCleanupTestCase::StopSenderAtEndpoint()
{
    m_senderStopNs = Simulator::Now().GetNanoSeconds();
    m_endpointOrder.push_back("sender-stop");
    m_fixture.sender->StopTraffic();
}

void
SaturatedTcpSinkEndpointCleanupTestCase::CaptureSinkSockets()
{
    m_listeningSocket = m_fixture.sink->GetListeningSocket();
    const auto acceptedSockets = m_fixture.sink->GetAcceptedSockets();
    if (!acceptedSockets.empty())
    {
        m_acceptedSocket = acceptedSockets.front();
    }
    if (auto tcp = DynamicCast<TcpSocketBase>(m_listeningSocket))
    {
        tcp->TraceConnectWithoutContext(
            "State",
            MakeCallback(&SaturatedTcpSinkEndpointCleanupTestCase::NotifySinkState, this)
                .Bind(true));
    }
    if (auto tcp = DynamicCast<TcpSocketBase>(m_acceptedSocket))
    {
        tcp->TraceConnectWithoutContext(
            "State",
            MakeCallback(&SaturatedTcpSinkEndpointCleanupTestCase::NotifySinkState, this)
                .Bind(false));
    }
}

void
SaturatedTcpSinkEndpointCleanupTestCase::NotifySinkState(bool listening,
                                                         TcpSocket::TcpStates_t oldState,
                                                         TcpSocket::TcpStates_t newState)
{
    if (Simulator::Now() != Seconds(2))
    {
        return;
    }
    if (listening)
    {
        m_listeningClosed = oldState == TcpSocket::LISTEN && newState == TcpSocket::CLOSED;
        if (m_listeningClosed)
        {
            m_endpointOrder.push_back("sink-stop");
        }
    }
    else
    {
        m_acceptedClosing =
            oldState == TcpSocket::ESTABLISHED && newState != TcpSocket::ESTABLISHED;
    }
}

void
SaturatedTcpSinkEndpointCleanupTestCase::ProbeConnectionSucceeded(Ptr<Socket> socket)
{
    m_probeConnected = true;
    socket->Send(Create<Packet>(512));
}

void
SaturatedTcpSinkEndpointCleanupTestCase::ProbeConnectionFailed(Ptr<Socket> socket)
{
    static_cast<void>(socket);
    m_probeFailed = true;
}

void
SaturatedTcpSinkEndpointCleanupTestCase::DoRun()
{
    Simulator::Destroy();
    m_fixture = BuildSenderFixture(true, Callback<void>());
    m_fixture.sink->SetStopTime(TimeStep(0));

    SaturatedReadinessBarrier barrier(
        MakeCallback(&SaturatedTcpSinkEndpointCleanupTestCase::StartStatistics, this),
        MakeCallback(&SaturatedTcpSinkEndpointCleanupTestCase::FinalizeStatistics, this));
    const auto ready = barrier.RegisterSender(
        m_fixture.sender,
        MakeCallback(&SaturatedTcpSender::StartTraffic, m_fixture.sender),
        MakeCallback(&SaturatedTcpSinkEndpointCleanupTestCase::StopSenderAtEndpoint, this));
    m_fixture.sender->SetReadyCallback(ready);
    barrier.RegisterApplication(m_fixture.sink);
    barrier.FinalizeRegistration();

    Simulator::Schedule(MilliSeconds(1500),
                        &SaturatedTcpSinkEndpointCleanupTestCase::CaptureSinkSockets,
                        this);
    Simulator::Run();

    m_probeSocket = Socket::CreateSocket(m_fixture.nodes.Get(0), TcpSocketFactory::GetTypeId());
    m_probeSocket->SetAttribute("ConnTimeout", TimeValue(MilliSeconds(10)));
    m_probeSocket->SetAttribute("ConnCount", UintegerValue(1));
    m_probeSocket->SetConnectCallback(
        MakeCallback(&SaturatedTcpSinkEndpointCleanupTestCase::ProbeConnectionSucceeded, this),
        MakeCallback(&SaturatedTcpSinkEndpointCleanupTestCase::ProbeConnectionFailed, this));
    NS_TEST_ASSERT_MSG_EQ(
        m_probeSocket->Bind(InetSocketAddress(m_fixture.interfaces.GetAddress(0), 11001)),
        0,
        "Could not bind post-end TCP probe");
    NS_TEST_ASSERT_MSG_EQ(
        m_probeSocket->Connect(InetSocketAddress(m_fixture.interfaces.GetAddress(1), 21000)),
        0,
        "Could not start post-end TCP probe");
    Simulator::Stop(MilliSeconds(100));
    Simulator::Run();
    m_bytesAfterProbe = m_fixture.sink->GetTotalRx();

    NS_TEST_ASSERT_MSG_EQ(m_statisticsStartNs,
                          Seconds(1).GetNanoSeconds(),
                          "Barrier selected the wrong sink-test epoch");
    NS_TEST_ASSERT_MSG_EQ(m_statisticsEndNs,
                          Seconds(2).GetNanoSeconds(),
                          "Barrier finalized sink test at the wrong endpoint");
    NS_TEST_ASSERT_MSG_EQ(m_listeningSocket != nullptr,
                          true,
                          "Packet sink had no listening socket before endpoint");
    NS_TEST_ASSERT_MSG_EQ(m_acceptedSocket != nullptr,
                          true,
                          "Packet sink had no accepted socket before endpoint");
    NS_TEST_ASSERT_MSG_EQ(m_senderStopNs,
                          Seconds(2).GetNanoSeconds(),
                          "Sender cleanup did not precede statistics finalization at endpoint");
    NS_TEST_ASSERT_MSG_EQ(m_listeningClosed,
                          true,
                          "Listening sink socket was not closed at endpoint");
    NS_TEST_ASSERT_MSG_EQ(m_acceptedClosing,
                          true,
                          "Accepted sink socket was not closed at endpoint");
    NS_TEST_ASSERT_MSG_EQ(m_probeConnected,
                          false,
                          "A post-end TCP connection reached the closed sink listener");
    NS_TEST_ASSERT_MSG_EQ(m_probeFailed, true, "Closed sink listener did not reject a connection");
    NS_TEST_ASSERT_MSG_EQ(m_bytesAfterProbe,
                          m_bytesAtEndpoint,
                          "Packet sink received payload after the measurement endpoint");
    NS_TEST_ASSERT_MSG_EQ(
        m_endpointOrder ==
            std::vector<std::string>({"sender-stop", "sink-stop", "statistics-finalize"}),
        true,
        "Endpoint cleanup order diverged from sender, sink, statistics");
    Simulator::Destroy();
}

/** Recorder for deterministic barrier start, stop, and statistics callbacks. */
class BarrierRecorder
{
  public:
    /**
     * Record one sender start.
     *
     * @param index Sender registration index.
     */
    void StartSender(uint32_t index);
    /**
     * Record one sender stop.
     *
     * @param index Sender registration index.
     */
    void StopSender(uint32_t index);
    /**
     * Record statistics start.
     *
     * @param epochNs Requested experiment epoch in nanoseconds.
     */
    void StartStatistics(int64_t epochNs);
    /**
     * Record statistics finalization.
     *
     * @param endNs Requested experiment endpoint in nanoseconds.
     */
    void FinalizeStatistics(int64_t endNs);

    std::array<uint32_t, 3> startCounts{};           ///< Start callbacks by registration.
    std::array<uint32_t, 3> stopCounts{};            ///< Stop callbacks by registration.
    std::array<int64_t, 3> startTimesNs{-1, -1, -1}; ///< Sender start timestamps.
    std::array<int64_t, 3> stopTimesNs{-1, -1, -1};  ///< Sender stop timestamps.
    uint32_t statisticsStartCount{0};                ///< Statistics start callback count.
    uint32_t statisticsFinalizeCount{0};             ///< Statistics finalize callback count.
    int64_t statisticsStartArgumentNs{-1};           ///< Statistics start callback argument.
    int64_t statisticsEndArgumentNs{-1};             ///< Statistics finalize callback argument.
    int64_t statisticsStartTimeNs{-1};               ///< Statistics start event timestamp.
    int64_t statisticsEndTimeNs{-1};                 ///< Statistics finalize event timestamp.
    std::vector<std::string> order;                  ///< Callback order at epoch boundaries.
};

void
BarrierRecorder::StartSender(uint32_t index)
{
    ++startCounts.at(index);
    startTimesNs.at(index) = Simulator::Now().GetNanoSeconds();
    order.push_back("sender-start-" + std::to_string(index));
}

void
BarrierRecorder::StopSender(uint32_t index)
{
    ++stopCounts.at(index);
    stopTimesNs.at(index) = Simulator::Now().GetNanoSeconds();
    order.push_back("sender-stop-" + std::to_string(index));
}

void
BarrierRecorder::StartStatistics(int64_t epochNs)
{
    ++statisticsStartCount;
    statisticsStartArgumentNs = epochNs;
    statisticsStartTimeNs = Simulator::Now().GetNanoSeconds();
    order.push_back("statistics-start");
}

void
BarrierRecorder::FinalizeStatistics(int64_t endNs)
{
    ++statisticsFinalizeCount;
    statisticsEndArgumentNs = endNs;
    statisticsEndTimeNs = Simulator::Now().GetNanoSeconds();
    order.push_back("statistics-finalize");
}

/**
 * @ingroup tests
 *
 * Verify the common next-second epoch and exact one-second measurement.
 */
class SaturatedReadinessBarrierEpochTestCase : public TestCase
{
  public:
    /** Construct the common-epoch test. */
    SaturatedReadinessBarrierEpochTestCase();

  private:
    void DoRun() override;
};

SaturatedReadinessBarrierEpochTestCase::SaturatedReadinessBarrierEpochTestCase()
    : TestCase("open saturated readiness barrier at one common whole-second epoch")
{
}

void
SaturatedReadinessBarrierEpochTestCase::DoRun()
{
    Simulator::Destroy();
    BarrierRecorder recorder;
    SaturatedReadinessBarrier barrier(
        MakeCallback(&BarrierRecorder::StartStatistics, &recorder),
        MakeCallback(&BarrierRecorder::FinalizeStatistics, &recorder));

    std::array<Ptr<SaturatedTcpSender>, 3> senderApplications;
    std::array<Callback<void>, 3> readinessCallbacks;
    for (uint32_t index = 0; index < senderApplications.size(); ++index)
    {
        senderApplications[index] = CreateObject<SaturatedTcpSender>();
        readinessCallbacks[index] = barrier.RegisterSender(
            senderApplications[index],
            MakeCallback(&BarrierRecorder::StartSender, &recorder).Bind(index),
            MakeCallback(&BarrierRecorder::StopSender, &recorder).Bind(index));
        senderApplications[index]->SetReadyCallback(readinessCallbacks[index]);
    }
    auto sinkApplication = CreateObject<PacketSink>();
    barrier.RegisterApplication(sinkApplication);
    barrier.FinalizeRegistration();

    Simulator::Schedule(MilliSeconds(1100), readinessCallbacks[0]);
    Simulator::Schedule(MilliSeconds(1250), readinessCallbacks[1]);
    Simulator::Schedule(MilliSeconds(1370), readinessCallbacks[2]);
    Simulator::Run();

    constexpr int64_t epochNs = 2'000'000'000;
    constexpr int64_t endNs = 3'000'000'000;
    NS_TEST_ASSERT_MSG_EQ(barrier.GetRegisteredSenderCount(), 3, "Wrong barrier sender count");
    NS_TEST_ASSERT_MSG_EQ(barrier.GetReadySenderCount(), 3, "Wrong barrier ready count");
    NS_TEST_ASSERT_MSG_EQ(barrier.GetRegisteredApplicationCount(),
                          4,
                          "Wrong coordinated application count");
    NS_TEST_ASSERT_MSG_EQ(barrier.GetExperimentStartNs(), epochNs, "Wrong common traffic epoch");
    NS_TEST_ASSERT_MSG_EQ(barrier.IsMeasurementComplete(), true, "Measurement did not finalize");
    NS_TEST_ASSERT_MSG_EQ(recorder.statisticsStartCount, 1, "Statistics started more than once");
    NS_TEST_ASSERT_MSG_EQ(recorder.statisticsFinalizeCount,
                          1,
                          "Statistics finalized more than once");
    NS_TEST_ASSERT_MSG_EQ(recorder.statisticsStartArgumentNs,
                          epochNs,
                          "Statistics received the wrong start epoch");
    NS_TEST_ASSERT_MSG_EQ(recorder.statisticsEndArgumentNs,
                          endNs,
                          "Statistics received the wrong endpoint");
    NS_TEST_ASSERT_MSG_EQ(recorder.statisticsStartTimeNs,
                          epochNs,
                          "Statistics did not start at the common epoch");
    NS_TEST_ASSERT_MSG_EQ(recorder.statisticsEndTimeNs,
                          endNs,
                          "Statistics did not finalize after exactly one second");
    NS_TEST_ASSERT_MSG_EQ(Simulator::Now().GetNanoSeconds(),
                          endNs,
                          "Simulator did not stop immediately after finalization");
    for (uint32_t index = 0; index < senderApplications.size(); ++index)
    {
        NS_TEST_ASSERT_MSG_EQ(recorder.startCounts[index],
                              1,
                              "Sender start callback count changed");
        NS_TEST_ASSERT_MSG_EQ(recorder.stopCounts[index], 1, "Sender stop callback count changed");
        NS_TEST_ASSERT_MSG_EQ(recorder.startTimesNs[index], epochNs, "Sender start epoch diverged");
        NS_TEST_ASSERT_MSG_EQ(recorder.stopTimesNs[index], endNs, "Sender stop epoch diverged");
        TimeValue stopTime;
        senderApplications[index]->GetAttribute("StopTime", stopTime);
        NS_TEST_ASSERT_MSG_EQ(stopTime.Get().GetNanoSeconds(),
                              endNs,
                              "Barrier did not coordinate sender application stop time");
    }
    TimeValue sinkStopTime;
    sinkApplication->GetAttribute("StopTime", sinkStopTime);
    NS_TEST_ASSERT_MSG_EQ(sinkStopTime.Get().GetNanoSeconds(),
                          endNs,
                          "Barrier did not coordinate sink application stop time");
    NS_TEST_ASSERT_MSG_EQ(recorder.order.front(),
                          "statistics-start",
                          "Senders started before statistics reset");
    NS_TEST_ASSERT_MSG_EQ(recorder.order.back(),
                          "statistics-finalize",
                          "Statistics finalized before sender cleanup");
    Simulator::Destroy();
}

/**
 * @ingroup tests
 *
 * Verify that destroying a barrier clears late sender readiness callbacks.
 */
class SaturatedReadinessBarrierLifetimeTestCase : public TestCase
{
  public:
    /** Construct the barrier callback-lifetime regression. */
    SaturatedReadinessBarrierLifetimeTestCase();

  private:
    /** Replace the active barrier with a different owner at the identical address. */
    void ReplaceBarrier();
    /** Record an unexpected replacement-sender start. */
    void StartReplacementSender();
    /** Record an unexpected replacement-sender stop. */
    void StopReplacementSender();
    /**
     * Ignore a statistics callback timestamp.
     *
     * @param timestampNs Absolute callback timestamp in nanoseconds.
     */
    void IgnoreStatistics(int64_t timestampNs);
    void DoRun() override;

    alignas(SaturatedReadinessBarrier) std::array<
        std::byte,
        sizeof(SaturatedReadinessBarrier)> m_barrierStorage{}; ///< Reused storage for exact-address
                                                               ///< ownership testing.
    SaturatedReadinessBarrier* m_barrier{nullptr};             ///< Current owner in reused storage.
    SenderFixture m_fixture;                                   ///< Delayed real TCP sender fixture.
    Ptr<SaturatedTcpSender> m_replacementSender; ///< Sender owned by the replacement barrier.
    uint32_t m_replacementStartCount{0};         ///< Replacement start callback count.
    uint32_t m_replacementStopCount{0};          ///< Replacement stop callback count.
    bool m_addressReused{false};                 ///< Whether replacement used the exact address.
};

SaturatedReadinessBarrierLifetimeTestCase::SaturatedReadinessBarrierLifetimeTestCase()
    : TestCase("clear late TCP readiness callback before barrier address reuse")
{
}

void
SaturatedReadinessBarrierLifetimeTestCase::ReplaceBarrier()
{
    const auto oldAddress = m_barrier;
    std::destroy_at(m_barrier);
    m_barrier = std::construct_at(
        reinterpret_cast<SaturatedReadinessBarrier*>(m_barrierStorage.data()),
        MakeCallback(&SaturatedReadinessBarrierLifetimeTestCase::IgnoreStatistics, this),
        MakeCallback(&SaturatedReadinessBarrierLifetimeTestCase::IgnoreStatistics, this));
    m_addressReused = oldAddress == m_barrier;

    m_replacementSender = CreateObject<SaturatedTcpSender>();
    const auto ready = m_barrier->RegisterSender(
        m_replacementSender,
        MakeCallback(&SaturatedReadinessBarrierLifetimeTestCase::StartReplacementSender, this),
        MakeCallback(&SaturatedReadinessBarrierLifetimeTestCase::StopReplacementSender, this));
    m_replacementSender->SetReadyCallback(ready);
    m_barrier->FinalizeRegistration();
}

void
SaturatedReadinessBarrierLifetimeTestCase::StartReplacementSender()
{
    ++m_replacementStartCount;
}

void
SaturatedReadinessBarrierLifetimeTestCase::StopReplacementSender()
{
    ++m_replacementStopCount;
}

void
SaturatedReadinessBarrierLifetimeTestCase::IgnoreStatistics(int64_t timestampNs)
{
    static_cast<void>(timestampNs);
}

void
SaturatedReadinessBarrierLifetimeTestCase::DoRun()
{
    Simulator::Destroy();
    m_fixture = BuildSenderFixture(true, Callback<void>());
    m_barrier = std::construct_at(
        reinterpret_cast<SaturatedReadinessBarrier*>(m_barrierStorage.data()),
        MakeCallback(&SaturatedReadinessBarrierLifetimeTestCase::IgnoreStatistics, this),
        MakeCallback(&SaturatedReadinessBarrierLifetimeTestCase::IgnoreStatistics, this));
    const auto ready = m_barrier->RegisterSender(m_fixture.sender,
                                                 MakeCallback(&IgnoreReady),
                                                 MakeCallback(&IgnoreReady));
    m_fixture.sender->SetReadyCallback(ready);
    m_barrier->FinalizeRegistration();

    Simulator::Schedule(NanoSeconds(100),
                        &SaturatedReadinessBarrierLifetimeTestCase::ReplaceBarrier,
                        this);
    Simulator::Stop(MilliSeconds(20));
    Simulator::Run();

    NS_TEST_ASSERT_MSG_EQ(m_addressReused, true, "Barrier replacement did not reuse its address");
    NS_TEST_ASSERT_MSG_EQ(m_barrier->GetReadySenderCount(),
                          0,
                          "Late TCP success reached the replacement barrier owner");
    NS_TEST_ASSERT_MSG_EQ(m_replacementStartCount,
                          0,
                          "Replacement barrier started from stale readiness");
    NS_TEST_ASSERT_MSG_EQ(m_replacementStopCount,
                          0,
                          "Replacement barrier stopped from stale readiness");

    std::destroy_at(m_barrier);
    m_barrier = nullptr;
    Simulator::Destroy();
}

/**
 * @ingroup tests
 *
 * Verify that an empty readiness barrier is rejected.
 */
class SaturatedReadinessBarrierEmptyTestCase : public TestCase
{
  public:
    /** Construct the empty-registration fatal-state test. */
    SaturatedReadinessBarrierEmptyTestCase();

  private:
    void DoRun() override;
};

SaturatedReadinessBarrierEmptyTestCase::SaturatedReadinessBarrierEmptyTestCase()
    : TestCase("reject empty saturated readiness barrier")
{
}

void
SaturatedReadinessBarrierEmptyTestCase::DoRun()
{
#ifdef __unix__
    const auto result = RunFatalPath([] {
        BarrierRecorder recorder;
        SaturatedReadinessBarrier barrier(
            MakeCallback(&BarrierRecorder::StartStatistics, &recorder),
            MakeCallback(&BarrierRecorder::FinalizeStatistics, &recorder));
        barrier.FinalizeRegistration();
    });
    NS_TEST_ASSERT_MSG_EQ(result.launched, true, "Could not create fatal-path child process");
    NS_TEST_ASSERT_MSG_EQ(result.failed, true, "Barrier accepted zero sender registrations");
    NS_TEST_ASSERT_MSG_NE(result.diagnostic.find("no saturated TCP senders were registered"),
                          std::string::npos,
                          "Empty-barrier failure omitted the registration diagnostic");
#endif
}

/**
 * @ingroup tests
 *
 * Verify that a readiness registration cannot report twice.
 */
class SaturatedReadinessBarrierDuplicateTestCase : public TestCase
{
  public:
    /** Construct the duplicate-readiness fatal-state test. */
    SaturatedReadinessBarrierDuplicateTestCase();

  private:
    void DoRun() override;
};

SaturatedReadinessBarrierDuplicateTestCase::SaturatedReadinessBarrierDuplicateTestCase()
    : TestCase("reject duplicate saturated sender readiness")
{
}

void
SaturatedReadinessBarrierDuplicateTestCase::DoRun()
{
#ifdef __unix__
    const auto result = RunFatalPath([] {
        BarrierRecorder recorder;
        SaturatedReadinessBarrier barrier(
            MakeCallback(&BarrierRecorder::StartStatistics, &recorder),
            MakeCallback(&BarrierRecorder::FinalizeStatistics, &recorder));
        auto firstSender = CreateObject<SaturatedTcpSender>();
        const auto first = barrier.RegisterSender(firstSender,
                                                  MakeCallback(&IgnoreReady),
                                                  MakeCallback(&IgnoreReady));
        firstSender->SetReadyCallback(first);
        auto secondSender = CreateObject<SaturatedTcpSender>();
        const auto second = barrier.RegisterSender(secondSender,
                                                   MakeCallback(&IgnoreReady),
                                                   MakeCallback(&IgnoreReady));
        secondSender->SetReadyCallback(second);
        barrier.FinalizeRegistration();
        Simulator::Schedule(Seconds(1), first);
        Simulator::Schedule(MilliSeconds(1100), first);
        Simulator::Run();
    });
    NS_TEST_ASSERT_MSG_EQ(result.launched, true, "Could not create fatal-path child process");
    NS_TEST_ASSERT_MSG_EQ(result.failed, true, "Barrier accepted duplicate sender readiness");
    NS_TEST_ASSERT_MSG_NE(result.diagnostic.find("reported readiness more than once"),
                          std::string::npos,
                          "Duplicate readiness failure omitted the sender diagnostic");
#endif
}

/**
 * @ingroup tests
 *
 * Verify that a complete replacement TCP cohort can still report readiness.
 */
class SaturatedReadinessBarrierDelayedReadyTestCase : public TestCase
{
  public:
    /** Construct the delayed-readiness regression test. */
    SaturatedReadinessBarrierDelayedReadyTestCase();

  private:
    void DoRun() override;
};

SaturatedReadinessBarrierDelayedReadyTestCase::SaturatedReadinessBarrierDelayedReadyTestCase()
    : TestCase("accept saturated readiness through one replacement TCP cohort")
{
}

void
SaturatedReadinessBarrierDelayedReadyTestCase::DoRun()
{
#ifdef __unix__
    const auto result = RunFatalPath([] {
        BarrierRecorder recorder;
        SaturatedReadinessBarrier barrier(
            MakeCallback(&BarrierRecorder::StartStatistics, &recorder),
            MakeCallback(&BarrierRecorder::FinalizeStatistics, &recorder));
        auto sender = CreateObject<SaturatedTcpSender>();
        const auto ready =
            barrier.RegisterSender(sender,
                                   MakeCallback(&BarrierRecorder::StartSender, &recorder).Bind(0),
                                   MakeCallback(&BarrierRecorder::StopSender, &recorder).Bind(0));
        sender->SetReadyCallback(ready);
        barrier.FinalizeRegistration();
        Simulator::Schedule(Seconds(382), ready);
        Simulator::Run();
        if (!barrier.IsMeasurementComplete() ||
            barrier.GetExperimentStartNs() != Seconds(383).GetNanoSeconds())
        {
            std::_Exit(EXIT_FAILURE);
        }
    });
    NS_TEST_ASSERT_MSG_EQ(result.launched,
                          true,
                          "Could not create delayed-readiness child process");
    NS_TEST_ASSERT_MSG_EQ(
        result.failed,
        false,
        "Readiness during a replacement TCP cohort was rejected: " << result.diagnostic);
#endif
}

/**
 * @ingroup tests
 *
 * Verify the fixed simulation-time safety failure for missing readiness.
 */
class SaturatedReadinessBarrierTimeoutTestCase : public TestCase
{
  public:
    /** Construct the missing-readiness timeout test. */
    SaturatedReadinessBarrierTimeoutTestCase();

  private:
    void DoRun() override;
};

SaturatedReadinessBarrierTimeoutTestCase::SaturatedReadinessBarrierTimeoutTestCase()
    : TestCase("fail saturated readiness after the post-retry safety timeout")
{
}

void
SaturatedReadinessBarrierTimeoutTestCase::DoRun()
{
#ifdef __unix__
    const auto result = RunFatalPath([] {
        BarrierRecorder recorder;
        SaturatedReadinessBarrier barrier(
            MakeCallback(&BarrierRecorder::StartStatistics, &recorder),
            MakeCallback(&BarrierRecorder::FinalizeStatistics, &recorder));
        auto sender = CreateObject<SaturatedTcpSender>();
        const auto ready =
            barrier.RegisterSender(sender, MakeCallback(&IgnoreReady), MakeCallback(&IgnoreReady));
        sender->SetReadyCallback(ready);
        barrier.FinalizeRegistration();
        Simulator::Run();
    });
    NS_TEST_ASSERT_MSG_EQ(result.launched, true, "Could not create fatal-path child process");
    NS_TEST_ASSERT_MSG_EQ(result.failed, true, "Missing readiness did not trigger safety failure");
    NS_TEST_ASSERT_MSG_NE(result.diagnostic.find("readiness timeout after "),
                          std::string::npos,
                          "Safety failure omitted the timeout diagnostic");
    NS_TEST_ASSERT_MSG_NE(result.diagnostic.find("0/1 senders ready"),
                          std::string::npos,
                          "Safety failure omitted the incomplete readiness count");
#endif
}

/**
 * Build three deterministic BSS endpoint groups for flow installation tests.
 *
 * @param stationCount Number of station endpoints in every BSS.
 * @return Three endpoint groups with unique nodes and IPv4 addresses.
 */
std::array<SaturatedTcpBssEndpoints, 3>
BuildTrafficEndpoints(uint32_t stationCount)
{
    std::array<SaturatedTcpBssEndpoints, 3> endpoints;
    for (uint32_t bssIndex = 0; bssIndex < endpoints.size(); ++bssIndex)
    {
        endpoints[bssIndex].server = {CreateObject<Node>(),
                                      Ipv4Address((10U << 24) | ((bssIndex + 1) << 16) | 1U)};
        for (uint32_t stationIndex = 0; stationIndex < stationCount; ++stationIndex)
        {
            endpoints[bssIndex].stations.push_back(
                {CreateObject<Node>(),
                 Ipv4Address((10U << 24) | ((bssIndex + 1) << 16) | (stationIndex + 10))});
        }
    }
    return endpoints;
}

/**
 * @ingroup tests
 *
 * Verify that maximum-matrix TCP setup starts after association without one synchronized burst.
 */
class SaturatedTcpFlowStartScheduleTestCase : public TestCase
{
  public:
    /** Construct the deterministic setup-schedule test. */
    SaturatedTcpFlowStartScheduleTestCase();

  private:
    void DoRun() override;
};

SaturatedTcpFlowStartScheduleTestCase::SaturatedTcpFlowStartScheduleTestCase()
    : TestCase("stage maximum saturated TCP flow setup after association")
{
}

void
SaturatedTcpFlowStartScheduleTestCase::DoRun()
{
    Simulator::Destroy();
    constexpr uint32_t stationCount = 30;
    auto endpoints = BuildTrafficEndpoints(stationCount);
    SaturatedTcpConfig config;
    config.benchmark.stationCountPerBss = stationCount;
    config.benchmark.trafficMode = SaturatedTrafficMode::UL_DL;

    BarrierRecorder recorder;
    SaturatedReadinessBarrier barrier(
        MakeCallback(&BarrierRecorder::StartStatistics, &recorder),
        MakeCallback(&BarrierRecorder::FinalizeStatistics, &recorder));
    const auto installation = InstallSaturatedTcpTraffic(endpoints, config, barrier);

    NS_TEST_ASSERT_MSG_EQ(installation.flows.size(), 180, "Wrong maximum-matrix flow count");
    for (uint32_t flowIndex = 0; flowIndex < installation.flows.size(); ++flowIndex)
    {
        TimeValue senderStart;
        installation.flows[flowIndex].sender->GetAttribute("StartTime", senderStart);
        NS_TEST_ASSERT_MSG_EQ(senderStart.Get(),
                              Seconds(1) + MilliSeconds(10 * flowIndex),
                              "Sender setup start was not deterministically staged");

        TimeValue sinkStart;
        installation.flows[flowIndex].sink->GetAttribute("StartTime", sinkStart);
        NS_TEST_ASSERT_MSG_EQ(sinkStart.Get(),
                              Seconds(0),
                              "Sink did not listen throughout staged TCP setup");
    }
    Simulator::Destroy();
}

/**
 * @ingroup tests
 *
 * Verify UL, DL, and bidirectional flow matrices across all three BSSs.
 */
class SaturatedTcpFlowMatrixTestCase : public TestCase
{
  public:
    /** Construct the deterministic flow-matrix test. */
    SaturatedTcpFlowMatrixTestCase();

  private:
    /**
     * Verify one traffic mode.
     *
     * @param mode Common mode for all three BSSs.
     * @param directionsPerStation Expected independent directions per station.
     */
    void CheckMode(SaturatedTrafficMode mode, uint32_t directionsPerStation);
    void DoRun() override;
};

SaturatedTcpFlowMatrixTestCase::SaturatedTcpFlowMatrixTestCase()
    : TestCase("install independent saturated TCP flow matrices for every BSS")
{
}

void
SaturatedTcpFlowMatrixTestCase::CheckMode(SaturatedTrafficMode mode, uint32_t directionsPerStation)
{
    Simulator::Destroy();
    constexpr uint32_t stationCount = 4;
    auto endpoints = BuildTrafficEndpoints(stationCount);
    SaturatedTcpConfig config;
    config.benchmark.stationCountPerBss = stationCount;
    config.benchmark.trafficMode = mode;
    config.tcp.segmentSizeBytes = 1460;

    BarrierRecorder recorder;
    SaturatedReadinessBarrier barrier(
        MakeCallback(&BarrierRecorder::StartStatistics, &recorder),
        MakeCallback(&BarrierRecorder::FinalizeStatistics, &recorder));
    const SaturatedTcpTrafficInstallation installation =
        InstallSaturatedTcpTraffic(endpoints, config, barrier);

    const uint32_t expectedFlowCount = 3 * stationCount * directionsPerStation;
    NS_TEST_ASSERT_MSG_EQ(installation.flows.size(),
                          expectedFlowCount,
                          "Traffic mode installed the wrong sender count");
    NS_TEST_ASSERT_MSG_EQ(barrier.GetRegisteredSenderCount(),
                          expectedFlowCount,
                          "A sender was not registered with the readiness barrier");
    NS_TEST_ASSERT_MSG_EQ(barrier.GetRegisteredApplicationCount(),
                          2 * expectedFlowCount,
                          "A source or sink was not coordinated by the barrier");

    std::array<uint32_t, 3> flowsPerBss{};
    std::array<std::array<std::array<uint32_t, 2>, stationCount>, 3> directionCounts{};
    std::set<const SaturatedTcpSender*> senders;
    std::set<const PacketSink*> sinks;
    std::set<uint16_t> sourcePorts;
    std::set<uint16_t> destinationPorts;
    std::set<std::pair<uint32_t, uint16_t>> sourceEndpoints;
    std::set<std::pair<uint32_t, uint16_t>> destinationEndpoints;
    for (const auto& flow : installation.flows)
    {
        ++flowsPerBss.at(flow.bssIndex);
        ++directionCounts.at(flow.bssIndex)
              .at(flow.stationIndex)
              .at(static_cast<uint32_t>(flow.direction));
        NS_TEST_ASSERT_MSG_EQ(senders.insert(PeekPointer(flow.sender)).second,
                              true,
                              "Two flows shared one sender application/TCP connection");
        NS_TEST_ASSERT_MSG_EQ(sinks.insert(PeekPointer(flow.sink)).second,
                              true,
                              "Two flows shared one packet sink");
        NS_TEST_ASSERT_MSG_EQ(sourcePorts.insert(flow.sourcePort).second,
                              true,
                              "Two flows shared one dedicated source port");
        NS_TEST_ASSERT_MSG_EQ(destinationPorts.insert(flow.destinationPort).second,
                              true,
                              "Two flows shared one dedicated destination port");
        NS_TEST_ASSERT_MSG_EQ(
            sourceEndpoints.insert({flow.sourceAddress.Get(), flow.sourcePort}).second,
            true,
            "Two flows shared one source endpoint");
        NS_TEST_ASSERT_MSG_EQ(
            destinationEndpoints.insert({flow.destinationAddress.Get(), flow.destinationPort})
                .second,
            true,
            "Two flows shared one destination endpoint");
        NS_TEST_ASSERT_MSG_NE(flow.sourcePort,
                              flow.destinationPort,
                              "A flow reused one port for source and destination");

        const auto& station = endpoints.at(flow.bssIndex).stations.at(flow.stationIndex);
        const auto& server = endpoints.at(flow.bssIndex).server;
        const bool uplink = flow.direction == SaturatedTcpFlowDirection::UL;
        const auto& expectedSource = uplink ? station : server;
        const auto& expectedDestination = uplink ? server : station;
        NS_TEST_ASSERT_MSG_EQ(flow.sender->GetNode(),
                              expectedSource.node,
                              "Sender was installed on the wrong endpoint node");
        NS_TEST_ASSERT_MSG_EQ(flow.sink->GetNode(),
                              expectedDestination.node,
                              "Sink was installed on the wrong endpoint node");
        NS_TEST_ASSERT_MSG_EQ(flow.sourceAddress,
                              expectedSource.address,
                              "Flow recorded the wrong source address");
        NS_TEST_ASSERT_MSG_EQ(flow.destinationAddress,
                              expectedDestination.address,
                              "Flow recorded the wrong destination address");

        const auto remote = InetSocketAddress::ConvertFrom(flow.sender->GetRemote());
        AddressValue localValue;
        flow.sender->GetAttribute("Local", localValue);
        const auto local = InetSocketAddress::ConvertFrom(localValue.Get());
        NS_TEST_ASSERT_MSG_EQ(local.GetIpv4(), flow.sourceAddress, "Sender local IP diverged");
        NS_TEST_ASSERT_MSG_EQ(local.GetPort(), flow.sourcePort, "Sender local port diverged");
        NS_TEST_ASSERT_MSG_EQ(remote.GetIpv4(),
                              flow.destinationAddress,
                              "Sender remote IP diverged");
        NS_TEST_ASSERT_MSG_EQ(remote.GetPort(),
                              flow.destinationPort,
                              "Sender remote port diverged");

        AddressValue sinkLocalValue;
        flow.sink->GetAttribute("Local", sinkLocalValue);
        const auto sinkLocal = InetSocketAddress::ConvertFrom(sinkLocalValue.Get());
        NS_TEST_ASSERT_MSG_EQ(sinkLocal.GetIpv4(),
                              flow.destinationAddress,
                              "Sink listened on the wrong destination IP");
        NS_TEST_ASSERT_MSG_EQ(sinkLocal.GetPort(),
                              flow.destinationPort,
                              "Sink listened on the wrong destination port");
    }
    for (uint32_t count : flowsPerBss)
    {
        NS_TEST_ASSERT_MSG_EQ(count,
                              stationCount * directionsPerStation,
                              "BSS did not use the common traffic mode");
    }
    for (const auto& bssDirections : directionCounts)
    {
        for (const auto& stationDirections : bssDirections)
        {
            NS_TEST_ASSERT_MSG_EQ(stationDirections[0],
                                  mode == SaturatedTrafficMode::DL ? 0 : 1,
                                  "Station received the wrong uplink flow count");
            NS_TEST_ASSERT_MSG_EQ(stationDirections[1],
                                  mode == SaturatedTrafficMode::UL ? 0 : 1,
                                  "Station received the wrong downlink flow count");
        }
    }
    Simulator::Destroy();
}

void
SaturatedTcpFlowMatrixTestCase::DoRun()
{
    CheckMode(SaturatedTrafficMode::UL, 1);
    CheckMode(SaturatedTrafficMode::DL, 1);
    CheckMode(SaturatedTrafficMode::UL_DL, 2);
}

} // namespace

std::vector<TestCase*>
CreateSaturatedTcpTrafficTestCases()
{
    return {new SaturatedTcpSenderLifecycleTestCase,
            new SaturatedTcpSenderBeforeReadyTestCase,
            new SaturatedTcpSenderDuplicateStartTestCase,
            new SaturatedTcpSenderConnectionRetryTestCase,
            new SaturatedTcpSenderRetryCancellationTestCase,
            new SaturatedTcpSinkEndpointCleanupTestCase,
            new SaturatedReadinessBarrierEpochTestCase,
            new SaturatedReadinessBarrierLifetimeTestCase,
            new SaturatedReadinessBarrierEmptyTestCase,
            new SaturatedReadinessBarrierDuplicateTestCase,
            new SaturatedReadinessBarrierDelayedReadyTestCase,
            new SaturatedReadinessBarrierTimeoutTestCase,
            new SaturatedTcpFlowStartScheduleTestCase,
            new SaturatedTcpFlowMatrixTestCase};
}
